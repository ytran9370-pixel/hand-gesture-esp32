"""
export_int8.py - Xuat model INT8 doi xung (W8A8) tu notebook Kaggle sang code C cho ESP32-S3.

Dan TOAN BO file nay vao 1 cell moi o CUOI notebook (sau CELL 13), roi chay:

    net = make_qnet(uniform_bits(8), "pct")      # PTQ W8A8 (test acc 94.61% trong notebook)
    # hoac: net = QAT_NETS["W8A8"]               # QAT W8A8 (94.24%)
    export_int8(net, TEST_X, TEST_Y, HAGRID_CLASSES, out_dir="/kaggle/working/esp32_int8")

Ket qua: /kaggle/working/esp32_int8.zip chua
    model_data.c   : trong so int8, bias int32, he so requant, bang LUT dau vao
    test_images.c  : 10 anh test (RGB 64x64) + logits mong doi (de kiem tra tren board)
    report.txt     : do chinh xac model so nguyen vs PyTorch

Luoc do luong tu hoa (giong het notebook):
    real = scale * q,   zero_point = 0
    weight: int8, scale theo tung kenh ra (per-channel), BN da gop vao conv
    activation: int8 [-128,127], 1 scale / tensor; sau ReLU -> [0,127]
    acc_int32 = sum(x_q * w_q) + bias_q,     bias_q = round(b / (s_in * s_w))
    y_q = clamp( round(acc * M), 0, 127 ),   M = s_in * s_w / s_out = M0 * 2^(shift-31)
"""
import os
import shutil

import numpy as np
import torch

LAYERS = [  # ten, kernel, stride, pad, maxpool 2x2 phia sau
    ("stem", 3, 2, 1, 0), ("stage1_conv", 3, 1, 1, 0), ("stage1_pw", 1, 1, 0, 1),
    ("stage2_conv", 3, 1, 1, 0), ("stage2_pw", 1, 1, 0, 1),
    ("stage3_conv", 3, 1, 1, 0), ("stage3_pw", 1, 1, 0, 1),
]


# ----------------------------------------------------------------------------
# 1) Phep toan fixed-point (giong TFLite / ESP-NN, va giong firmware C)
# ----------------------------------------------------------------------------
def quantize_multiplier(m):
    """M (float, >0) -> (M0 int32 trong [2^30, 2^31), shift) voi M = M0 * 2^(shift-31)."""
    if m <= 0:
        return 0, 0
    q, shift = np.frexp(m)
    q0 = int(round(q * (1 << 31)))
    if q0 == (1 << 31):
        q0 //= 2
        shift += 1
    return q0, int(shift)


def requant(acc, mult, shift):
    """acc (int64 array, kenh o truc cuoi) * M -> int64, lam tron giong het code C."""
    mult = np.asarray(mult, np.int64)
    shift = np.asarray(shift, np.int64)
    x = acc.astype(np.int64) << np.maximum(shift, 0)
    ab = x * mult
    v = ab + np.where(ab >= 0, 1 << 30, 1 - (1 << 30))
    hi = np.where(v >= 0, v >> 31, -((-v) >> 31))            # chia 2^31 cat ve 0 (nhu C)
    e = np.maximum(-shift, 0)
    mask = (np.int64(1) << e) - 1
    res = hi >> e
    return res + ((hi & mask) > ((mask >> 1) + (res < 0)))


# ----------------------------------------------------------------------------
# 2) Lay tham so so nguyen tu QNet
# ----------------------------------------------------------------------------
def act_scale(sd, key):
    return sd[key].float().cpu().clamp(min=1e-8) / 127.0     # = ActQuant.scale() (signed int8)


def build_params(net):
    sd = {k: v.detach().float().cpu() for k, v in net.state_dict().items()}
    s_in = act_scale(sd, "in_aq.running_max")
    x = (torch.arange(256, dtype=torch.float32) / 255.0 - 0.5) / 0.5      # ToTensor + Normalize(0.5,0.5)
    P = {"lut": torch.clamp(torch.round(x / s_in), -128, 127).to(torch.int8).numpy(), "convs": []}
    s_prev, hw = s_in, 64
    for name, k, st, pad, pool in LAYERS:
        w, b = sd[f"layers.{name}.weight"], sd[f"layers.{name}.bias"]
        s_w = w.reshape(w.shape[0], -1).abs().max(1).values.clamp(min=1e-8) / 127.0   # minmax per-channel
        wq = torch.clamp(torch.round(w / s_w.view(-1, 1, 1, 1)), -128, 127).to(torch.int8)
        s_out = act_scale(sd, f"layers.{name}.aq.running_max")
        s_acc = s_prev.double() * s_w.double()
        bq = torch.round(b.double() / s_acc).to(torch.int64).numpy().astype(np.int32)
        mult, shift = zip(*[quantize_multiplier(float(m)) for m in (s_acc / s_out.double()).numpy()])
        out_hw = (hw + 2 * pad - k) // st + 1
        P["convs"].append(dict(name=name, k=k, stride=st, pad=pad, pool=pool, in_hw=hw, out_hw=out_hw,
                               cin=w.shape[1], cout=w.shape[0],
                               w=wq.permute(0, 2, 3, 1).contiguous().numpy(),   # OIHW -> OHWI
                               b=bq, mult=np.array(mult, np.int32), shift=np.array(shift, np.int32)))
        s_prev, hw = s_out, (out_hw // 2 if pool else out_hw)
    s_gap = act_scale(sd, "gap_aq.running_max")
    P["gap_hw"] = hw
    P["gap_mult"], P["gap_shift"] = quantize_multiplier(float(s_prev.double() / (hw * hw) / s_gap.double()))
    w, b = sd["layers.fc.weight"], sd["layers.fc.bias"]
    s_fc = w.abs().max().clamp(min=1e-8) / 127.0                       # FC: 1 scale cho ca tensor
    P["fc_w"] = torch.clamp(torch.round(w / s_fc), -128, 127).to(torch.int8).numpy()
    P["fc_b"] = torch.round(b.double() / (s_gap.double() * s_fc.double())).to(torch.int64).numpy().astype(np.int32)
    P["logit_scale"] = float(s_gap.double() * s_fc.double())
    return P


# ----------------------------------------------------------------------------
# 3) Model so nguyen bang NumPy (= "golden model", firmware phai ra dung y het)
# ----------------------------------------------------------------------------
def run_int(P, rgb):
    x = P["lut"][rgb].astype(np.int32)                                 # (64,64,3) HWC
    for L in P["convs"]:
        H, k, s, p = x.shape[0], L["k"], L["stride"], L["pad"]
        Ho = L["out_hw"]
        xp = np.pad(x, ((p, p), (p, p), (0, 0)))
        cols = np.stack([xp[ky:ky + s * Ho:s, kx:kx + s * Ho:s] for ky in range(k) for kx in range(k)], 2)
        acc = cols.reshape(Ho * Ho, -1).astype(np.int64) @ L["w"].reshape(L["cout"], -1).T.astype(np.int64)
        y = np.clip(requant(acc + L["b"], L["mult"], L["shift"]), 0, 127).reshape(Ho, Ho, -1)
        if L["pool"]:
            y = y.reshape(Ho // 2, 2, Ho // 2, 2, -1).max(axis=(1, 3))
        x = y
    g = np.clip(requant(x.sum(axis=(0, 1)), P["gap_mult"], P["gap_shift"]), -128, 127)
    return (P["fc_w"].astype(np.int64) @ g + P["fc_b"]).astype(np.int32)


# ----------------------------------------------------------------------------
# 4) Sinh code C
# ----------------------------------------------------------------------------
def c_arr(ctype, name, a, per_line=16):
    a = np.asarray(a).ravel()
    rows = ["    " + ", ".join(str(int(v)) for v in a[i:i + per_line]) + "," for i in range(0, len(a), per_line)]
    return f"const {ctype} {name}[{len(a)}] = {{\n" + "\n".join(rows) + "\n};\n"


def write_model_c(P, labels, path):
    out = ["/* TU DONG SINH boi export_int8.py - khong sua tay */", '#include "model.h"', "",
           c_arr("int8_t", "g_input_lut", P["lut"])]
    for L in P["convs"]:
        n = L["name"]
        out += [f"/* {n}: {L['cout']}x{L['k']}x{L['k']}x{L['cin']} (OHWI) */",
                "static " + c_arr("int8_t", f"{n}_w", L["w"], 32), "static " + c_arr("int32_t", f"{n}_b", L["b"], 8),
                "static " + c_arr("int32_t", f"{n}_mult", L["mult"], 8),
                "static " + c_arr("int32_t", f"{n}_shift", L["shift"], 16)]
    out.append("const conv_layer_t g_conv_layers[NUM_CONV_LAYERS] = {")
    for L in P["convs"]:
        n = L["name"]
        out.append(f'    {{"{n}", {L["k"]}, {L["stride"]}, {L["pad"]}, {L["pool"]}, {L["in_hw"]}, {L["cin"]}, '
                   f'{L["out_hw"]}, {L["cout"]}, {n}_w, {n}_b, {n}_mult, {n}_shift}},')
    out += ["};", "", f"const int32_t g_gap_mult = {P['gap_mult']};", f"const int32_t g_gap_shift = {P['gap_shift']};",
            c_arr("int8_t", "g_fc_w", P["fc_w"], 32), c_arr("int32_t", "g_fc_b", P["fc_b"], 9),
            f"const float g_logit_scale = {P['logit_scale']:.9e}f;",
            "const char *const g_labels[NUM_CLASSES] = {" + ", ".join(f'"{c}"' for c in labels) + "};"]
    open(path, "w").write("\n".join(out) + "\n")


def write_test_c(rgb, labels, logits, path):
    out = ["/* TU DONG SINH boi export_int8.py: anh test + logits mong doi tu golden model NumPy */",
           '#include "model.h"', "", f"const int g_num_test = {len(rgb)};",
           c_arr("uint8_t", "g_test_label", labels, 32), c_arr("int32_t", "g_test_logits", logits, 9),
           c_arr("uint8_t", "g_test_rgb", rgb, 48)]
    open(path, "w").write("\n".join(out) + "\n")


def export_int8(net, test_x, test_y, labels, out_dir="/kaggle/working/esp32_int8", n_embed=10):
    os.makedirs(out_dir, exist_ok=True)
    net.eval()
    P = build_params(net)
    # anh test: tensor da Normalize -> dao nguoc lai ve pixel uint8 (chinh xac tuyet doi)
    rgb = np.clip(np.round((test_x.cpu().numpy() * 0.5 + 0.5) * 255), 0, 255).astype(np.uint8).transpose(0, 2, 3, 1)
    y = test_y.cpu().numpy()
    print(f"Chay model so nguyen tren {len(rgb)} anh test ...")
    logits = np.stack([run_int(P, r) for r in rgb])
    pred_int = logits.argmax(1)
    dev = next(net.parameters()).device
    with torch.no_grad():
        pred_pt = torch.cat([net(test_x[i:i + 512].to(dev)).argmax(1).cpu() for i in range(0, len(test_x), 512)]).numpy()
    rep = (f"PyTorch fake-quant acc : {(pred_pt == y).mean():.4f}\n"
           f"Integer (C/NumPy) acc  : {(pred_int == y).mean():.4f}\n"
           f"Trung du doan          : {(pred_pt == pred_int).mean():.4f}\n")
    print(rep)
    idx = np.random.default_rng(0).choice(len(rgb), n_embed, replace=False)
    write_model_c(P, labels, os.path.join(out_dir, "model_data.c"))
    write_test_c(rgb[idx], y[idx], logits[idx], os.path.join(out_dir, "test_images.c"))
    open(os.path.join(out_dir, "report.txt"), "w").write(rep)
    shutil.make_archive(out_dir, "zip", out_dir)
    print(f"Xong -> tai {out_dir}.zip o tab Output, giai nen vao firmware/main/")

import torch, torch.nn as nn, torch.nn.functional as F
from einops import rearrange
from muscriptor.models.lm import LMModel, TorchAutocast
from muscriptor.modules.conditioners import (
    ConditioningProvider, MelSpectrogramConditioner, ClassConditioner, WavCondition,
)
from safetensors.torch import load_file

DIM, HEADS, LAYERS, CARD = 768, 12, 14, 1393   # "small" — from HF config.json
HEAD_DIM = DIM // HEADS


def build_model(weights_path=None):
    mel = MelSpectrogramConditioner(output_dim=DIM, device="cpu", sample_rate=16000,
                                     n_fft=2048, frame_rate=100, n_mel_bins=512)
    inst = ClassConditioner(num_classes=1000, output_dim=DIM, device="cpu")
    ds = ClassConditioner(num_classes=4, output_dim=DIM, device="cpu")
    provider = ConditioningProvider(
        conditioners={"self_wav": mel, "instrument_group": inst, "dataset_name": ds},
        device="cpu",
    )
    model = LMModel(condition_provider=provider, card=CARD, dim=DIM, num_heads=HEADS,
                     hidden_scale=4, cfg_coef=1.0, autocast=TorchAutocast(enabled=False),
                     num_layers=LAYERS, max_period=10000, device="cpu")
    if weights_path:
        state_dict = load_file(weights_path, device="cpu")
        if any(k.startswith(("emb.0.", "linears.0.")) for k in state_dict):
            state_dict = {
                k.replace("emb.0.", "emb.").replace("linears.0.", "linear."): v
                for k, v in state_dict.items()
            }
        model.load_state_dict(state_dict)
    model.eval()
    return model


def attn_step(layer, x, past_k, past_v):
    attn = layer.self_attn
    projected = F.linear(x, attn.in_proj_weight)
    packed = rearrange(projected, "b t (p h d) -> b t p h d", p=3, h=HEADS)
    q, k, v = packed.unbind(dim=2)
    k = torch.cat([past_k, k], dim=1)
    v = torch.cat([past_v, v], dim=1)
    q_t, k_t, v_t = q.transpose(1, 2), k.transpose(1, 2), v.transpose(1, 2)
    T_q, T_k = q_t.shape[2], k_t.shape[2]
    if T_q == T_k:
        out = F.scaled_dot_product_attention(q_t, k_t, v_t, is_causal=True, dropout_p=0.0)
    else:
        out = F.scaled_dot_product_attention(q_t, k_t, v_t, dropout_p=0.0)
    out = rearrange(out.transpose(1, 2), "b t h d -> b t (h d)")
    return attn.out_proj(out), k, v


def sin_embedding(positions, dim, max_period=10000.0):
    half = dim // 2
    adim = torch.arange(half, dtype=torch.float32).view(1, 1, -1)
    phase = positions.float() / (max_period ** (adim / (half - 1)))
    return torch.cat([torch.cos(phase), torch.sin(phase)], dim=-1)


class PrefillGraph(nn.Module):
    """cond + BOS -> first-token logits + initial per-layer KV cache."""
    def __init__(self, model):
        super().__init__()
        self.m = model

    def forward(self, self_wav, instrument_group, dataset_name):
        m = self.m
        wav_cond = WavCondition(wav=self_wav, length=torch.tensor([self_wav.shape[-1]]), sample_rate=[16000])
        mel_embed, _ = m.condition_provider.conditioners["self_wav"](wav_cond)
        inst_embed, _ = m.condition_provider.conditioners["instrument_group"](instrument_group)
        ds_embed, _ = m.condition_provider.conditioners["dataset_name"](dataset_name)

        B = self_wav.shape[0]
        bos = torch.full((B, 1), m.initial_token_id, dtype=torch.long)
        x = torch.cat([mel_embed, inst_embed, ds_embed, m.emb(bos)], dim=1)
        T = x.shape[1]
        x = x + sin_embedding(torch.arange(T).view(1, -1, 1), DIM).to(x.dtype)

        ks, vs = [], []
        for layer in m.transformer.layers:
            zero_kv = torch.zeros(B, 0, HEADS, HEAD_DIM, dtype=x.dtype)
            attn_out, k, v = attn_step(layer, layer.norm1(x), zero_kv, zero_kv)
            x = x + attn_out
            x = x + layer.linear2(F.gelu(layer.linear1(layer.norm2(x))))
            ks.append(k); vs.append(v)

        logits = m.linear(m.out_norm(x)[:, -1:])
        return (logits, *ks, *vs)


class StepGraph(nn.Module):
    """one token + per-layer past KV -> next-token logits + updated per-layer KV."""
    def __init__(self, model):
        super().__init__()
        self.m = model

    def forward(self, token, position, *past_kv):
        m = self.m
        past_k, past_v = past_kv[:LAYERS], past_kv[LAYERS:]
        x = m.emb(token) + sin_embedding(position.view(1, -1, 1), DIM).to(m.emb.weight.dtype)

        ks, vs = [], []
        for i, layer in enumerate(m.transformer.layers):
            attn_out, k, v = attn_step(layer, layer.norm1(x), past_k[i], past_v[i])
            x = x + attn_out
            x = x + layer.linear2(F.gelu(layer.linear1(layer.norm2(x))))
            ks.append(k); vs.append(v)

        logits = m.linear(m.out_norm(x))
        return (logits, *ks, *vs)


if __name__ == "__main__":
    model = build_model("model.safetensors")
    prefill, step = PrefillGraph(model), StepGraph(model)
    kv_names = [f"k{i}" for i in range(LAYERS)] + [f"v{i}" for i in range(LAYERS)]

    self_wav = torch.randn(1, 1, 80000)          # 5s @ 16kHz
    instrument_group = torch.tensor([[5, 12, 30]])
    dataset_name = torch.tensor([[0]])

    torch.onnx.export(
        prefill, (self_wav, instrument_group, dataset_name), "prefill.onnx",
        input_names=["self_wav", "instrument_group", "dataset_name"],
        output_names=["logits", *kv_names],
        dynamic_axes={"self_wav": {2: "samples"},
                       "instrument_group": {1: "num_instruments"},
                       "logits": {0: "batch"},
                       **{n: {1: "cond_len"} for n in kv_names}},
        opset_version=18,
    )

    dummy_past = [torch.zeros(1, 5, HEADS, HEAD_DIM) for _ in range(2 * LAYERS)]
    past_len = torch.export.Dim("past_len")
    dynamic_shapes = ({}, {}, tuple({1: past_len} for _ in range(2 * LAYERS)))
    torch.onnx.export(
        step, (torch.tensor([[1]]), torch.tensor([5]), *dummy_past), "step.onnx",
        input_names=["token", "position", *kv_names],
        output_names=["logits", *[f"new_{n}" for n in kv_names]],
        dynamic_shapes=dynamic_shapes, opset_version=18,
    )
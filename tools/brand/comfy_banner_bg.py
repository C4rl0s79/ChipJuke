"""ChipJuke - tlo banera z lokalnego ComfyUI (SDXL). Opcjonalne narzedzie autora; wynik jest w repo.

Uzycie:  python -I tools/brand/comfy_banner_bg.py <plik.png> [seed]
Wymaga:  ComfyUI na 127.0.0.1:8188 z checkpointem Juggernaut-XI-byRunDiffusion.safetensors.
Uzyte w docs/images: seed 1123335343, 1536x640, 32 kroki dpmpp_2m karras, cfg 5.5.
"""
import json
import sys
import time
import urllib.parse
import urllib.request

HOST = "http://127.0.0.1:8188"
PROMPT = ("wide cinematic background, dark navy room, abstract glowing neon arch shape in mint green, "
          "rows of thin oscilloscope waveforms in mint and cyan, soft amber light strips like tracker rows, bokeh, "
          "subtle CRT scanlines, retro chiptune arcade mood, lots of empty dark space on the right side, minimal, no text")
NEG = "text, letters, words, watermark, signature, logo text, blurry, lowres, jpeg artifacts, photo of person, hands"


def main():
    out = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1123335343
    g = {
        "1": {"class_type": "CheckpointLoaderSimple", "inputs": {"ckpt_name": "Juggernaut-XI-byRunDiffusion.safetensors"}},
        "2": {"class_type": "CLIPTextEncode", "inputs": {"text": PROMPT, "clip": ["1", 1]}},
        "3": {"class_type": "CLIPTextEncode", "inputs": {"text": NEG, "clip": ["1", 1]}},
        "4": {"class_type": "EmptyLatentImage", "inputs": {"width": 1536, "height": 640, "batch_size": 1}},
        "5": {"class_type": "KSampler", "inputs": {"model": ["1", 0], "positive": ["2", 0], "negative": ["3", 0],
              "latent_image": ["4", 0], "seed": seed, "steps": 32, "cfg": 5.5,
              "sampler_name": "dpmpp_2m", "scheduler": "karras", "denoise": 1.0}},
        "6": {"class_type": "VAEDecode", "inputs": {"samples": ["5", 0], "vae": ["1", 2]}},
        "7": {"class_type": "SaveImage", "inputs": {"images": ["6", 0], "filename_prefix": "chipjuke_bg"}},
    }
    req = urllib.request.Request(HOST + "/prompt", data=json.dumps({"prompt": g}).encode(),
                                 headers={"Content-Type": "application/json"})
    pid = json.load(urllib.request.urlopen(req))["prompt_id"]
    while True:
        h = json.load(urllib.request.urlopen(f"{HOST}/history/{pid}"))
        if pid in h:
            break
        time.sleep(2)
    for node in h[pid]["outputs"].values():
        for im in node.get("images", []):
            q = urllib.parse.urlencode({"filename": im["filename"], "subfolder": im["subfolder"], "type": im["type"]})
            open(out, "wb").write(urllib.request.urlopen(f"{HOST}/view?{q}").read())
            print("ok", out, seed)
            return


if __name__ == "__main__":
    main()

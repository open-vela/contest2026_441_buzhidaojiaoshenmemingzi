#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Restore/check a pinned public ASR model. No executable downloads or audio I/O."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-root", type=Path, required=True)
    parser.add_argument("--restore", action="store_true", help="download missing public assets over HTTPS")
    args = parser.parse_args()
    lock = json.loads(Path(__file__).with_name("local-asr-model.json").read_text())
    root = args.model_root.resolve()
    if args.restore:
        root.mkdir(parents=True, exist_ok=True)
    for item in lock["files"]:
        path = root / item["path"]
        path.resolve().relative_to(root)
        if not path.exists() and args.restore:
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".partial")
            try:
                url = f'https://huggingface.co/{lock["repository"]}/resolve/{lock["revision"]}/{item["path"]}'
                with urllib.request.urlopen(url, timeout=60) as source, temporary.open("xb") as target:
                    while block := source.read(1024 * 1024): target.write(block)
                if (temporary.stat().st_size != item["bytes"] or
                        hashlib.sha256(temporary.read_bytes()).hexdigest() != item["sha256"]):
                    raise ValueError("downloaded ASR asset failed checksum")
                temporary.replace(path)
            finally:
                if temporary.exists(): temporary.unlink()
        if (not path.is_file() or path.stat().st_size != item["bytes"] or
                hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]):
            raise ValueError("ASR asset missing/modified (never overwritten): " + item["path"])
    # Same vocabulary export as the pinned sherpa-onnx export_bpe_vocab.py.
    # SentencePiece only reads the already authenticated model, no training.
    vocab = root / "bpe.vocab"
    if not vocab.exists() and args.restore:
        import sentencepiece as spm
        sp = spm.SentencePieceProcessor(model_file=str(root / "bpe.model"))
        text = "".join(f"{sp.id_to_piece(i)}\t{sp.get_score(i)}\n" for i in range(sp.get_piece_size()))
        vocab.write_text(text, encoding="utf-8", newline="\n")
    if not vocab.is_file() or hashlib.sha256(vocab.read_bytes()).hexdigest() != lock["generated_bpe_vocab_sha256"]:
        raise ValueError("generated ASR vocabulary missing/modified")
    if args.restore:
        (root / "verified-inputs.json").write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
    print("LOCAL_ASR_ASSETS_PASS", lock["revision"], "; no microphone, speaker or cloud API")


if __name__ == "__main__":
    main()

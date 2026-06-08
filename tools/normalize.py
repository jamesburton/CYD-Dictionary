"""Search-key normalisation shared by the dictionary builders and verifier.
MUST match the C++ implementation in src/Dict.cpp (dictNormalizeKey)."""
import re
import unicodedata

_KEEP = re.compile(r"[^a-z0-9 \-]")
_WS = re.compile(r"\s+")


def norm_key(s: str) -> str:
    s = unicodedata.normalize("NFKD", s)
    s = "".join(c for c in s if not unicodedata.combining(c))  # strip accents
    s = s.lower().replace("'", "").replace("’", "")        # drop apostrophes
    s = _KEEP.sub(" ", s)                                       # other punct -> space
    s = _WS.sub(" ", s).strip()
    return s

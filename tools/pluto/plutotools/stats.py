"""Small statistics helpers (nearest-rank percentiles, summaries, deltas)."""

import math


def pctl(values, p):
    """Nearest-rank percentile of a list (p in 0..100); None when empty."""
    v = sorted(x for x in values if x is not None)
    if not v:
        return None
    k = max(1, int(math.ceil(p / 100.0 * len(v))))
    return v[k - 1]


def summary(values):
    """{n, mean, p50, p99, max, min} of the non-None values."""
    v = [x for x in values if x is not None]
    if not v:
        return {"n": 0, "mean": None, "p50": None, "p99": None, "max": None, "min": None}
    return {
        "n": len(v),
        "mean": sum(v) / len(v),
        "p50": pctl(v, 50),
        "p99": pctl(v, 99),
        "max": max(v),
        "min": min(v),
    }


def delta(a, b):
    """b - a for numbers, None if either is missing."""
    if a is None or b is None:
        return None
    return b - a


def pct_change(a, b):
    if a in (None, 0) or b is None:
        return None
    return 100.0 * (b - a) / abs(a)


def fmt(v, digits=1):
    if v is None:
        return "-"
    if isinstance(v, bool):
        return "yes" if v else "no"
    if isinstance(v, int):
        return str(v)
    return ("%." + str(digits) + "f") % v

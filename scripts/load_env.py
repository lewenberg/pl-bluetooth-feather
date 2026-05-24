import json
from pathlib import Path

Import("env")

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
ENV_FILE = PROJECT_DIR / ".env"

REQUIRED_KEYS = (
    "WIFI_PASSWORD",
    "WIFI_TARGET_BSSID",
)


def parse_env_line(line):
    key, value = line.split("=", 1)
    key = key.strip()
    value = value.strip()

    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        value = value[1:-1]

    return key, value


def load_dotenv(path):
    values = {}
    if not path.exists():
        raise RuntimeError(f"Missing Wi-Fi config file: {path}")

    for raw_line in path.read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise RuntimeError(f"Bad .env line: {raw_line}")

        key, value = parse_env_line(line)
        values[key] = value

    missing = [key for key in REQUIRED_KEYS if not values.get(key)]
    if missing:
        raise RuntimeError(f"Missing required .env keys: {', '.join(missing)}")

    return values


wifi_config = load_dotenv(ENV_FILE)

generated_dir = PROJECT_DIR / ".pio" / "generated"
generated_dir.mkdir(parents=True, exist_ok=True)

header = generated_dir / "wifi_config.h"
header.write_text(
    "#pragma once\n\n"
    + "\n".join(
        f"#define {key} {json.dumps(value)}"
        for key, value in wifi_config.items()
        if key.startswith("WIFI_")
    )
    + "\n"
)

env.Append(CPPPATH=[str(generated_dir)])

import json
import socket
from pathlib import Path

Import("env")


def read_dotenv(path):
    values = {}

    for line_number, raw_line in enumerate(path.read_text().splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        if line.startswith("export "):
            line = line[len("export ") :].lstrip()

        if "=" not in line:
            raise RuntimeError(f"Invalid .env assignment on line {line_number}")

        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()

        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]

        values[key] = value

    return values


def read_int(values, key, default, minimum, maximum):
    raw_value = values.get(key, str(default))
    try:
        value = int(raw_value)
    except ValueError as error:
        raise RuntimeError(f"{key} must be an integer") from error

    if not minimum <= value <= maximum:
        raise RuntimeError(f"{key} must be between {minimum} and {maximum}")
    return value


def read_bool(values, key, default):
    raw_value = values.get(key, str(default)).strip().lower()
    if raw_value in ("1", "true", "yes", "on"):
        return True
    if raw_value in ("0", "false", "no", "off"):
        return False
    raise RuntimeError(f"{key} must be true or false")


def detect_lan_ip():
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
            connection.connect(("8.8.8.8", 80))
            return connection.getsockname()[0]
    except OSError as error:
        raise RuntimeError(
            "Could not detect this computer's LAN IP; set MQTT_HOST in .env"
        ) from error


project_dir = Path(env.subst("$PROJECT_DIR"))
dotenv_path = project_dir / ".env"

if not dotenv_path.is_file():
    raise RuntimeError(
        "Missing .env file. Copy .env.example and set WIFI_NAME and PASSWORD."
    )

config = read_dotenv(dotenv_path)
required_keys = ("WIFI_NAME", "PASSWORD")
missing_keys = [key for key in required_keys if not config.get(key)]

if missing_keys:
    raise RuntimeError("Missing or empty .env keys: " + ", ".join(missing_keys))

mqtt_host = config.get("MQTT_HOST") or detect_lan_ip()
mqtt_port = read_int(config, "MQTT_PORT", 1883, 1, 65535)
mqtt_topic = config.get("MQTT_TOPIC", "lights/rgb")
red_pin = read_int(config, "RGB_RED_PIN", 32, 0, 39)
green_pin = read_int(config, "RGB_GREEN_PIN", 33, 0, 39)
blue_pin = read_int(config, "RGB_BLUE_PIN", 25, 0, 39)
common_anode = read_bool(config, "RGB_COMMON_ANODE", False)
status_led_pin = read_int(config, "STATUS_LED_PIN", 2, 0, 33)
status_led_active_low = read_bool(config, "STATUS_LED_ACTIVE_LOW", False)

if len({red_pin, green_pin, blue_pin}) != 3:
    raise RuntimeError("RGB_RED_PIN, RGB_GREEN_PIN, and RGB_BLUE_PIN must differ")

input_only_pins = {34, 35, 36, 37, 38, 39}
invalid_output_pins = input_only_pins.intersection({red_pin, green_pin, blue_pin})
if invalid_output_pins:
    invalid_list = ", ".join(str(pin) for pin in sorted(invalid_output_pins))
    raise RuntimeError(f"RGB pins must support output; input-only pin(s): {invalid_list}")

if status_led_pin in {red_pin, green_pin, blue_pin}:
    raise RuntimeError("STATUS_LED_PIN must differ from all RGB output pins")

generated_dir = project_dir / ".pio" / "generated"
generated_dir.mkdir(parents=True, exist_ok=True)
header_path = generated_dir / "wifi_config.h"
header = "\n".join(
    (
        "#pragma once",
        "",
        "namespace wifi_config {",
        f"constexpr char kSsid[] = {json.dumps(config['WIFI_NAME'])};",
        f"constexpr char kPassword[] = {json.dumps(config['PASSWORD'])};",
        f"constexpr char kMqttHost[] = {json.dumps(mqtt_host)};",
        f"constexpr uint16_t kMqttPort = {mqtt_port};",
        f"constexpr char kMqttTopic[] = {json.dumps(mqtt_topic)};",
        f"constexpr uint8_t kRedPin = {red_pin};",
        f"constexpr uint8_t kGreenPin = {green_pin};",
        f"constexpr uint8_t kBluePin = {blue_pin};",
        f"constexpr bool kCommonAnode = {'true' if common_anode else 'false'};",
        f"constexpr uint8_t kStatusLedPin = {status_led_pin};",
        f"constexpr bool kStatusLedActiveLow = {'true' if status_led_active_low else 'false'};",
        "}  // namespace wifi_config",
        "",
    )
)

if not header_path.exists() or header_path.read_text() != header:
    header_path.write_text(header)

env.Append(CPPPATH=[str(generated_dir)])

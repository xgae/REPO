#!/usr/bin/env bash
set -euo pipefail
# setup_attendance_env.sh
# Prepares a minimal "VS Code-like" environment using VSCodium + PlatformIO CLI for ESP8266
# - Minimal extensions only
# - No PlatformIO extension (keeps no watchers/background)
# - Portable Codium profile option
# - Creates project skeleton in ~/esp-attendance
# - Installs platformio via pipx, esptool via pacman, pio libs for project
# - Adds udev rule for CH340 and adds user to serial group (uucp / plugdev)
#
# Run as a normal user. Script will call sudo for package installs & udev.

export PROJECT_DIR="$HOME/esp-attendance"
export CODIUM_PORTABLE_DIR="$HOME/codium-portable"
export VSCODIUM_SETTINGS_DIR="$CODIUM_PORTABLE_DIR/User"
export CODIUM_BIN="codium"   # binary name
export PIO_BIN="$(command -v pio || true)"

# Colors
GREEN='\033[0;32m'; RED='\033[0;31m'; YEL='\033[1;33m'; NC='\033[0m'

echo -e "${GREEN}Starting environment setup for ESP8266 (Wemos D1 R1) and VSCodium (pure mode).${NC}"
echo

# 1) Ensure pacman packages required
echo -e "${YEL}Installing required system packages (pacman): git base-devel cmake python-pipx esptool usbutils picocom...${NC}"
sudo pacman -Syu --noconfirm
sudo pacman -S --needed --noconfirm git base-devel cmake python-pipx esptool usbutils picocom

# 2) Ensure pipx is available & platformio installed via pipx
if ! command -v pipx >/dev/null 2>&1; then
  echo -e "${YEL}pipx not found; installing python-pipx via pacman...${NC}"
  sudo pacman -S --noconfirm python-pipx
fi

echo -e "${YEL}Ensuring pipx PATHs are available in shell...${NC}"
export PATH="$HOME/.local/bin:$PATH"

if ! command -v pio >/dev/null 2>&1; then
  echo -e "${YEL}Installing PlatformIO Core via pipx (safe, isolated)...${NC}"
  pipx install platformio || { echo -e "${RED}pipx install platformio failed${NC}"; exit 1; }
else
  echo -e "${GREEN}PlatformIO (pio) already installed: $(pio --version)${NC}"
fi

# 3) Ensure VSCodium present, otherwise install AUR prebuilt or portable
if command -v ${CODIUM_BIN} >/dev/null 2>&1; then
  echo -e "${GREEN}VSCodium already installed: $(command -v ${CODIUM_BIN})${NC}"
else
  echo -e "${YEL}VSCodium not found. Attempting to install vscodium-bin via AUR helper 'yay' if available...${NC}"
  if command -v yay >/dev/null 2>&1; then
    yay -S --noconfirm vscodium-bin
  else
    echo -e "${YEL}No AUR helper found. Installing portable VSCodium into ${CODIUM_PORTABLE_DIR} ...${NC}"
    mkdir -p "$CODIUM_PORTABLE_DIR"
    cd "$CODIUM_PORTABLE_DIR"
    # Download latest release tarball from VSCodium GitHub releases (best effort)
    TARURL="$(curl -s https://api.github.com/repos/VSCodium/vscodium/releases/latest | grep browser_download_url | grep 'linux-x64' | head -n1 | cut -d '\"' -f4)"
    if [ -z "$TARURL" ]; then
      echo -e "${RED}Could not find VSCodium release tarball URL. Install manually via AUR or package manager.${NC}"
    else
      echo -e "${YEL}Downloading: $TARURL${NC}"
      curl -L -o vscodium.tar.gz "$TARURL"
      tar xzf vscodium.tar.gz --strip-components=1
      rm -f vscodium.tar.gz
      echo -e "${GREEN}Portable VSCodium extracted to: ${CODIUM_PORTABLE_DIR}${NC}"
      echo -e "${YEL}You can run it with: ${CODIUM_PORTABLE_DIR}/VSCodium --user-data-dir ${CODIUM_PORTABLE_DIR}/User --extensions-dir ${CODIUM_PORTABLE_DIR}/extensions${NC}"
    fi
    cd - >/dev/null || true
  fi
fi

# 4) Install minimal Codium extensions (if codium installed)
if command -v ${CODIUM_BIN} >/dev/null 2>&1; then
  echo -e "${YEL}Installing minimal Codium extensions (C/C++ + Catppuccin theme)...${NC}"
  ${CODIUM_BIN} --install-extension ms-vscode.cpptools >/dev/null 2>&1 || true
  ${CODIUM_BIN} --install-extension catppuccin.catppuccin-vsc >/dev/null 2>&1 || true
fi

# 5) Create portable settings (so nothing gets written to global config if you want)
echo -e "${YEL}Writing portable VSCodium settings to ${VSCODIUM_SETTINGS_DIR}/settings.json ...${NC}"
mkdir -p "$VSCODIUM_SETTINGS_DIR"
cat > "$VSCODIUM_SETTINGS_DIR/settings.json" <<'JSON'
{
  "workbench.colorTheme": "Catppuccin Mocha",
  "editor.fontFamily": "JetBrains Mono, Fira Code, monospace",
  "editor.fontLigatures": true,
  "editor.minimap.enabled": false,
  "editor.renderWhitespace": "boundary",
  "editor.cursorBlinking": "smooth",
  "workbench.startupEditor": "none",
  "telemetry.telemetryLevel": "off",
  "extensions.ignoreRecommendations": true,
  "files.trimTrailingWhitespace": true,
  "files.insertFinalNewline": true,
  "terminal.integrated.fontFamily": "JetBrains Mono",
  "terminal.integrated.fontSize": 13
}
JSON

# 6) Create project skeleton & move .ino if found
echo -e "${YEL}Initializing project at ${PROJECT_DIR} ...${NC}"
mkdir -p "$PROJECT_DIR"
cd "$PROJECT_DIR"
if [ ! -f platformio.ini ]; then
  pio project init --board d1
fi

# Move user's .ino file if present in Downloads
if [ -f "$HOME/Downloads/attendance_enhanced.ino" ] && [ ! -f src/main.cpp ]; then
  mkdir -p src
  echo -e "${YEL}Moving ~/Downloads/attendance_enhanced.ino -> ${PROJECT_DIR}/src/main.cpp${NC}"
  mv "$HOME/Downloads/attendance_enhanced.ino" src/main.cpp
fi

# 7) Ensure platformio.ini contains required libs and settings (overwrite safe)
cat > platformio.ini <<'INI'
[env:d1]
platform = espressif8266
board = d1
framework = arduino
monitor_speed = 115200
upload_speed = 115200

lib_deps =
  Adafruit Fingerprint Sensor Library
  LiquidCrystal_I2C
INI

# 8) Create .vscode/tasks.json for Codium tasks (calls CLI)
mkdir -p .vscode
cat > .vscode/tasks.json <<'JSON'
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "PIO: Build",
      "type": "shell",
      "command": "pio run",
      "group": { "kind": "build", "isDefault": true },
      "presentation": { "panel": "shared", "echo": true }
    },
    {
      "label": "PIO: Upload",
      "type": "shell",
      "command": "pio run -t upload --upload-port ${input:uploadPort}",
      "presentation": { "panel": "shared", "echo": true }
    },
    {
      "label": "PIO: Monitor",
      "type": "shell",
      "command": "pio device monitor --port ${input:uploadPort} --baud 115200",
      "presentation": { "panel": "shared", "echo": true }
    }
  ],
  "inputs": [
    {
      "id": "uploadPort",
      "type": "promptString",
      "description": "Serial port (e.g. /dev/ttyUSB0)",
      "default": "/dev/ttyUSB0"
    }
  ]
}
JSON

# 9) Makefile convenience
cat > Makefile <<'MAKE'
.PHONY: build upload monitor clean

build:
	pio run

upload:
	@if [ -z "$(PORT)" ]; then echo "Usage: make upload PORT=/dev/ttyUSB0"; exit 1; fi
	pio run -t upload --upload-port $(PORT)

monitor:
	@if [ -z "$(PORT)" ]; then echo "Usage: make monitor PORT=/dev/ttyUSB0"; exit 1; fi
	pio device monitor --port $(PORT) --baud 115200

clean:
	pio run -t clean
MAKE

# 10) Ensure required project libraries installed via pio
echo -e "${YEL}Installing required libraries into the project via PlatformIO CLI...${NC}"
pio lib install "Adafruit Fingerprint Sensor Library" || true
pio lib install "LiquidCrystal_I2C" || true

# 11) Create udev rules for CH340 and common FTDI; add current user to plugdev/uucp if necessary
echo -e "${YEL}Creating udev rule for common USB-serial devices (CH340 / FTDI).${NC}"
RULEFILE="/etc/udev/rules.d/99-esp-attendance-serial.rules"
sudo tee "$RULEFILE" > /dev/null <<'RULE'
# CH340 (QinHeng) and FTDI common rules for serial permissions
ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE="0666", GROUP="plugdev"
ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", MODE="0666", GROUP="plugdev"
RULE
sudo udevadm control --reload-rules && sudo udevadm trigger

# Add user to plugdev or uucp group if present
if getent group plugdev >/dev/null; then
  echo -e "${YEL}Adding $USER to group plugdev ...${NC}"
  sudo usermod -aG plugdev "$USER" || true
elif getent group uucp >/dev/null; then
  echo -e "${YEL}Adding $USER to group uucp ...${NC}"
  sudo usermod -aG uucp "$USER" || true
else
  echo -e "${YEL}No plugdev/uucp group found; skipping usermod. If permission errors appear, run uploads with sudo or create a group manually.${NC}"
fi

# 12) Final verification steps
echo
echo -e "${GREEN}Setup finished.${NC}"
echo
echo -e "${GREEN}Summary:${NC}"
echo "- Project directory: ${PROJECT_DIR}"
echo "- VSCodium (editor): $(command -v ${CODIUM_BIN} || echo \"portable at ${CODIUM_PORTABLE_DIR}\")"
echo "- PlatformIO (CLI): $(command -v pio || echo \"not found\")"
echo "- PlatformIO version: $(pio --version || true)"
echo "- Installed libs for project: Adafruit Fingerprint, LiquidCrystal_I2C"
echo
echo -e "${YEL}What to do next (copy/paste):${NC}"
echo "1) Open the project in VSCodium (pure editor):"
echo "   codium ${PROJECT_DIR} --user-data-dir ${CODIUM_PORTABLE_DIR}/User --extensions-dir ${CODIUM_PORTABLE_DIR}/extensions &"
echo
echo "2) Build:"
echo "   cd ${PROJECT_DIR} && pio run"
echo
echo "3) Upload (auto port detection):"
echo "   cd ${PROJECT_DIR} && pio run -t upload"
echo "   Or explicitly:"
echo "   pio run -t upload --upload-port /dev/ttyUSB0"
echo
echo "4) Monitor serial:"
echo "   pio device monitor --port /dev/ttyUSB0 --baud 115200"
echo
echo -e "${YEL}Important notes:${NC}"
echo "- We did NOT install the PlatformIO VS Code extension. This keeps VSCodium pure and no background services will run when you close the editor."
echo "- Use the tasks.json (Ctrl+Shift+B) in VSCodium to trigger the CLI tasks or use the Makefile / terminal."
echo "- If you just added yourself to plugdev/uucp, log out/login or run 'newgrp plugdev' to activate the group membership."
echo
echo -e "${GREEN}If anything fails, paste the exact error output here and I will debug step-by-step.${NC}"

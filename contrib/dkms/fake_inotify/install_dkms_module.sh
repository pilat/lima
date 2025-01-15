#!/bin/bash

MODULE_NAME="fake_inotify"
MODULE_VERSION="1.0"
DKMS_DIR="/usr/src/${MODULE_NAME}-${MODULE_VERSION}"

# Detect the Linux distribution
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO=$ID
else
    echo "Unsupported OS. Unable to detect distribution."
    exit 1
fi

echo "Detected Linux distribution: $DISTRO"

# Install required packages based on the distribution
case "$DISTRO" in
    ubuntu|debian)
        echo "Installing dependencies for Ubuntu/Debian..."
        sudo apt update
        sudo apt install -y dkms build-essential linux-headers-$(uname -r)
        ;;
    fedora)
        echo "Installing dependencies for Fedora..."
        sudo dnf install -y dkms gcc make kernel-devel kernel-headers
        ;;
    centos|rocky|almalinux)
        echo "Installing dependencies for CentOS/Rocky Linux/AlmaLinux..."
        sudo yum install -y epel-release
        sudo yum install -y dkms gcc make kernel-devel kernel-headers
        ;;
    arch|manjaro)
        echo "Installing dependencies for Arch/Manjaro..."
        sudo pacman -Syu --noconfirm
        sudo pacman -S --noconfirm dkms linux-headers base-devel
        ;;
    opensuse|sles)
        echo "Installing dependencies for openSUSE/SLES..."
        sudo zypper ref
        sudo zypper install -y dkms gcc make kernel-devel kernel-headers
        ;;
    *)
        echo "Unsupported distribution: $DISTRO"
        exit 1
        ;;
esac

# Copy the source files to /usr/src
echo "Copying source files to $DKMS_DIR..."
sudo cp -r . "$DKMS_DIR"

# Delete old dkms module if it exists
if [ -d "/var/lib/dkms/${MODULE_NAME}" ]; then
    echo "Removing old DKMS module..."
    sudo dkms remove -m "$MODULE_NAME" -v "$MODULE_VERSION" --all
fi

# Add, build, and install the module with DKMS
echo "Adding module to DKMS..."
sudo dkms add -m "$MODULE_NAME" -v "$MODULE_VERSION"

echo "Building module with DKMS..."
sudo dkms build -m "$MODULE_NAME" -v "$MODULE_VERSION"

echo "Installing module with DKMS..."
sudo dkms install -m "$MODULE_NAME" -v "$MODULE_VERSION"

echo "Unloading the old module if it's already loaded..."
sudo rmmod "$MODULE_NAME" 2>/dev/null

# Load the module immediately
echo "Loading the module immediately..."
if sudo modprobe "$MODULE_NAME"; then
    echo "Module $MODULE_NAME loaded successfully."
else
    echo "Failed to load module $MODULE_NAME. Please check the logs."
    exit 1
fi

# Configure the module to load at boot
echo "Configuring the module to load at boot..."

# Method 1: Use modules-load.d
MODULES_LOAD_CONF="/etc/modules-load.d/${MODULE_NAME}.conf"
echo "Adding $MODULE_NAME to $MODULES_LOAD_CONF..."
echo "$MODULE_NAME" | sudo tee "$MODULES_LOAD_CONF"

# Method 2: Use /etc/modules (fallback)
MODULES_FILE="/etc/modules"
if [ -f "$MODULES_FILE" ]; then
    if ! grep -q "^$MODULE_NAME$" "$MODULES_FILE"; then
        echo "Adding $MODULE_NAME to $MODULES_FILE..."
        echo "$MODULE_NAME" | sudo tee -a "$MODULES_FILE"
    fi
fi

# Method 3: Custom init script (for legacy systems)
INIT_SCRIPT="/etc/init.d/load_${MODULE_NAME}"
if [ ! -f "$INIT_SCRIPT" ]; then
    echo "Creating custom init script at $INIT_SCRIPT..."
    sudo tee "$INIT_SCRIPT" > /dev/null <<EOF
#!/bin/sh
### BEGIN INIT INFO
# Provides:          load_${MODULE_NAME}
# Required-Start:    \$local_fs
# Required-Stop:
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Load ${MODULE_NAME} module at boot
### END INIT INFO

case "\$1" in
    start)
        modprobe ${MODULE_NAME}
        ;;
    stop)
        rmmod ${MODULE_NAME}
        ;;
    *)
        echo "Usage: \$0 {start|stop}"
        exit 1
        ;;
esac

exit 0
EOF
    sudo chmod +x "$INIT_SCRIPT"
    sudo update-rc.d load_${MODULE_NAME} defaults
fi

# Verify the module is loaded
echo "Verifying the module is loaded..."
if lsmod | grep -q "$MODULE_NAME"; then
    echo "Module $MODULE_NAME successfully loaded and configured to autoload at boot."
else
    echo "Module $MODULE_NAME failed to load. Please check the logs."
fi

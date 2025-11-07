#!/bin/bash
# Test script for 3-buffer alpha blending
# Run this on the target device (T113-S3)

echo "========================================="
echo "  G2D 3-Buffer Alpha Blend Test"
echo "========================================="
echo

# Unload existing module
echo "1. Unloading existing sunxi-g2d module..."
sudo rmmod sunxi-g2d 2>/dev/null
sleep 1

# Load new module
echo "2. Loading new sunxi-g2d module..."
sudo insmod /home/spuc/sunxi-g2d.ko
if [ $? -ne 0 ]; then
    echo "❌ Failed to load module!"
    exit 1
fi
echo "✅ Module loaded"
sleep 1

# Clear kernel log
echo "3. Clearing kernel log..."
sudo dmesg -c > /dev/null

# Run test
echo "4. Running demo-3buffer-test..."
echo
sudo /home/spuc/demo-3buffer-test 2>&1

# Show relevant kernel messages
echo
echo "========================================="
echo "  Kernel Messages"
echo "========================================="
sudo dmesg | grep -E "MIXER|BLD|timeout|Alpha blend|V0|UI2|WB"

echo
echo "Test complete!"

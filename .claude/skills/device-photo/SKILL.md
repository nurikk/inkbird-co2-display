---
name: device-photo
description: Capture a photo of the physical device display using the Mac camera. Use when debugging TFT display issues, verifying UI rendering, documenting visual bugs, or when the user says "take a photo", "capture the display", "show me the screen", or wants to see what the device looks like.
allowed-tools: Bash, Read
---

# Device Photo Capture

Capture photos of the physical ESP32 device display for visual troubleshooting and verification.

## Quick Start

```bash
# With custom filename (if $ARGUMENTS provided)
imagesnap -d "FaceTime HD Camera" -w 1.0 $ARGUMENTS

# Default: timestamped filename
imagesnap -d "FaceTime HD Camera" -w 1.0 device_$(date +%Y%m%d_%H%M%S).jpg
```

## Command Options

| Option | Description |
|--------|-------------|
| `-d "FaceTime HD Camera"` | Camera device (Mac built-in) |
| `-w 1.0` | Warmup delay (seconds) for camera initialization |
| `filename.jpg` | Output file (use `$ARGUMENTS` or timestamped default) |

## When to Use

- Debugging TFT display output issues
- Documenting visual bugs or UI problems
- Verifying the physical display matches expected rendering
- Before/after comparisons when making UI changes
- When the user wants to see what the device currently shows

## Workflow

1. **Capture the photo** (use `$ARGUMENTS` if provided, otherwise generate timestamped name):
   ```bash
   # If user provides filename: /device-photo my_photo.jpg
   imagesnap -d "FaceTime HD Camera" -w 1.0 my_photo.jpg

   # Otherwise use timestamped default
   imagesnap -d "FaceTime HD Camera" -w 1.0 device_$(date +%Y%m%d_%H%M%S).jpg
   ```

2. **Read/analyze the captured image** using the Read tool to check:
   - Correct text rendering
   - Layout alignment
   - Color accuracy
   - UI element positioning
   - Any visual artifacts or glitches

3. **Report findings** to the user with specific observations about:
   - What's displayed correctly
   - Any issues identified
   - Recommendations for fixes if problems are found

## Tips

- The `-w 1.0` warmup delay helps ensure consistent image quality
- Ensure good lighting on the device display
- Position the camera to minimize glare/reflections
- Take multiple photos if needed for different states
- Use descriptive filenames: `display_before_fix.jpg`, `display_after_fix.jpg`
- Clean up photo files after troubleshooting session

## Example Invocations

```
/device-photo                     # Uses timestamped filename
/device-photo my_capture.jpg      # Custom filename
/device-photo before_fix.jpg      # Descriptive name for comparison
```

## Example Workflow

User: "The display looks wrong, can you check it?"

1. Capture: `imagesnap -d "FaceTime HD Camera" -w 1.0 display_check.jpg`
2. Analyze with Read tool
3. Identify issues (misaligned text, wrong colors, etc.)
4. Suggest code fixes based on visual analysis

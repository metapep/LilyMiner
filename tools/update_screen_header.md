# Update Screen Header Tool

Use `tools/update_screen_header.py` to regenerate `src/media/images_*.h` arrays from image files.

## 1) Inspect expected arrays

```bash
python3 tools/update_screen_header.py --header src/media/images_320_170.h --list
```

This prints array names (for example `initScreen`, `MinerScreen`) and expected dimensions when available.

## 2) Update from a folder

Put images in a folder using array names as file names:

- `initScreen.png`
- `MinerScreen.png`
- `setupModeScreen.png`
- etc.

Then run:

```bash
python3 tools/update_screen_header.py \
  --header src/media/images_320_170.h \
  --images-dir /path/to/my/320x170-images
```

## 3) Update specific arrays only

```bash
python3 tools/update_screen_header.py \
  --header src/media/images_320_170.h \
  --map initScreen=/path/to/init.png \
  --map MinerScreen=/path/to/miner.png \
  --require-all-mapped
```

## Notes

- The script converts pixels to RGB565 and writes `PROGMEM` values.
- It validates image dimensions against `*Width/*Height` constants when those constants exist.
- Without Pillow installed, the script uses macOS `sips` as fallback.

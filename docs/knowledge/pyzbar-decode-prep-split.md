# Splitting pyzbar's prep from its zbar scan without forking pyzbar

Timing "how long does a QR decode take" on the v0.8.7 Pi build runs into a boundary problem:
`pyzbar.decode()` is a single call that does two very different things, and only one of them is the
thing being compared against the native pipeline.

## What `pyzbar.decode()` actually does

In SeedSigner's pyzbar fork (`SeedSigner/pyzbar` v0.1.9-ss, the version Buildroot installs), the
whole of `decode()` is:

```python
def decode(image, symbols=None, binary=False, x_density=None, y_density=None):
    pixels, width, height = _pixel_data(image)

    results = []
    with _image_scanner() as scanner:
        ...
```

So it is **prep, then scan**. `_pixel_data()` is the prep: given the `(480, 480, 3)` uint8 array
picamera delivers, it reduces the frame to an 8bpp buffer zbar can consume.

Two details of `_pixel_data` matter and neither is obvious from the call site:

1. **For a 3-channel numpy array it takes the first channel, not a luma conversion:**

   ```python
   if 3 == len(image.shape):
       # Take just the first channel
       image = image[:, :, 0]
   ```

   With `format="rgb"` from picamera, that is the **R channel**. Anything comparing this against a
   pipeline that feeds zbar a **luma (Y)** plane is comparing the same pixel count and depth but
   different pixel *content*. That is a QR-readability difference (contrast against the background),
   not a CPU-cost difference — so it belongs next to the success ratio, not next to the decode time.

2. **It passes a `(pixels, width, height)` tuple straight through.** The final branch of
   `_pixel_data` is:

   ```python
   else:
       # image should be a tuple (pixels, width, height)
       pixels, width, height = image
       # ... cheap dimension and bits-per-pixel checks only
   ```

## The consequence: a clean split, no fork

Because of (2), the two segments can be timed separately with **no duplicated work and no copy of
pyzbar's code**:

```python
t0 = perf_counter_ns()
pixel_data = _pixel_data(image)          # prep segment
t1 = perf_counter_ns()
barcodes = pyzbar.decode(pixel_data, symbols=[ZBarSymbol.QRCODE], binary=is_binary)
t2 = perf_counter_ns()                   # decode segment
```

The `_pixel_data()` call inside `decode()` now hits the tuple branch, which is a couple of integer
checks. The conversion happens exactly once, in the segment that is supposed to own it.

The naive alternatives are both wrong in ways that are easy to miss:

- Timing `_pixel_data(image)` and then calling `decode(image)` does the conversion **twice** — the
  prep cost lands in both segments and the total is inflated by a full conversion.
- Timing the whole `decode()` as "decode time" folds prep into it, which is not the boundary the
  native side reports (it times the `zbar_scan_image` call with its stride-strip accounted
  separately). Two builds measured at different boundaries cannot be subtracted.

## Why this is worth knowing

`docs/qr-scan-camera-perf-benchmark-todo.md` had "collect the pre-processing segment **only if** the
prep is a cleanly isolatable boundary — otherwise skip it rather than report an asymmetric number."
The tuple passthrough is what makes it isolatable, so the segment is collected on both builds and
the pre-processing win is quantified rather than assumed.

Note the prep segment is not symmetric in *content* across builds even though it is symmetric in
*boundary*: on the native pipeline it is a strided-Y → contiguous-Y copy, here it is a channel
extraction from an interleaved RGB array. Both are "turn the delivered frame into the buffer zbar
consumes", which is the comparison that was wanted.

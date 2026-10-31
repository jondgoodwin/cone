# safetensors

Reads [safetensors](https://github.com/huggingface/safetensors) files: Hugging Face's tensor container, eight bytes of header
length, a JSON header, then the tensors' raw bytes. No GPU; depends on `collections`, `json`, `fs` and `path`.

```cone
import safetensors use *;

let file = match openSafetensors(&Path.from("model.safetensors")) { case var Ok(f) { f } case let Err(e) { ... } };   // e: SafetensorsError; e.message() says why
match file.header.find("layers.0.weight") {
  case let Some(t) { /* t.dtype, t.shape, t.rows(), t.cols(), t.begin, t.end */ }
  case is None {}
}
let w = file.header.floats(file.bytes(), "layers.0.weight");   // Option[List[f32]]
```

- **Dtypes:** F32, F16, BF16 and I32 (`ST_F32`, `ST_F16`, `ST_BF16`, `ST_I32`). Any other dtype is refused by name.
  `floats` widens F16 and BF16 exactly; `ints` reads an I32 tensor. `bytesOf` gives a tensor's raw bytes (little-endian,
  row-major) for a GPU upload as they are; `SafetensorsFile.base()` is the address of the mapped file.
- **What is checked:** the header length is within the file; the header is a JSON object; each tensor has a known dtype,
  a shape of whole numbers and a pair of offsets within the data, and its byte length is its dtype's size times its
  shape's product. Tensors need not be in order or contiguous.
- **`__metadata__`** is read as text pairs (`header.meta("key")`).
- **Not here:** writing safetensors files, and F64/I64/8-bit dtypes.

Tests: `tests/reader.cone` builds files byte by byte (and one on disk, mapped), reads every dtype, and checks refusals.

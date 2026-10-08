/** The size of a JPEG picture, read from its header (the SOF marker). Null, if it is not a JPEG. */
export function jpegSize(data: Buffer): { width: number; height: number } | null {
  if (data.length < 4 || data[0] !== 0xff || data[1] !== 0xd8) {
    return null;
  }

  let offset = 2;
  while (offset + 9 < data.length) {
    if (data[offset] !== 0xff) {
      offset++;
      continue;
    }

    const marker = data[offset + 1]!;
    // Padding, and markers without a length.
    if (marker === 0xff || marker === 0x01 || (marker >= 0xd0 && marker <= 0xd8)) {
      offset += marker === 0xff ? 1 : 2;
      continue;
    }

    const length = data.readUInt16BE(offset + 2);
    // The "start of frame" markers carry the size (not DHT 0xc4, JPG 0xc8, DAC 0xcc).
    if (marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc) {
      return { height: data.readUInt16BE(offset + 5), width: data.readUInt16BE(offset + 7) };
    }

    offset += 2 + length;
  }

  return null;
}

//! MPD HERMES FrontPanel transport decoder.

#![allow(unsafe_code)]

#[cfg(not(target_endian = "little"))]
compile_error!("the HERMES decoder currently requires a little-endian target");

use std::ffi::{CStr, c_char};
use std::ptr;

use pipewireao_spa_node::{
    Factory, Format, FormatConstraint, InputFrame, Node, OutputFrame, Port, PortRef, Rate, sys,
};

/// Factory name of the HERMES raw-batch decoder.
pub const HERMES_DECODER_FACTORY_NAME: &str = "api.hermes.decoder";
/// Exact semantic schema of FrontPanel carrier batches.
pub const HERMES_RAW_BATCH_SCHEMA: &str = "org.pipewireao.hermes.frontpanel-raw-batch/1";
/// Exact semantic schema of decoded counter-frame batches.
pub const HERMES_DECODED_BATCH_SCHEMA: &str = "org.pipewireao.hermes.counter-frame-batch/1";

const KEY_FRAMES: &[u8] = b"api.hermes.frames-per-buffer\0";
const KEY_COUNTERS: &[u8] = b"api.hermes.counters\0";
const KEY_BITS: &[u8] = b"api.hermes.bits-per-pixel\0";
const KEY_HALF: &[u8] = b"api.hermes.half-array\0";
const KEY_RATE: &[u8] = b"api.hermes.batch-rate\0";

const INPUT: usize = 0;
const OUTPUT: usize = 1;

static FACTORY: Factory = Factory::new::<HermesDecoderNode>(b"api.hermes.decoder\0");

fn optional_info<'a>(info: Option<&'a sys::spa_dict>, key: &[u8]) -> Result<Option<&'a str>, i32> {
    let key = CStr::from_bytes_with_nul(key).map_err(|_| -libc::EINVAL)?;
    let Some(info) = info else {
        return Ok(None);
    };
    if info.n_items != 0 && info.items.is_null() {
        return Err(-libc::EINVAL);
    }
    let items = unsafe { std::slice::from_raw_parts(info.items, info.n_items as usize) };
    for item in items {
        if item.key.is_null() || item.value.is_null() {
            continue;
        }
        let item_key = unsafe { CStr::from_ptr(item.key.cast::<c_char>()) };
        if item_key == key {
            return unsafe { CStr::from_ptr(item.value.cast::<c_char>()) }
                .to_str()
                .map(Some)
                .map_err(|_| -libc::EINVAL);
        }
    }
    Ok(None)
}

fn positive_u32(info: Option<&sys::spa_dict>, key: &[u8], default: u32) -> Result<u32, i32> {
    let value = optional_info(info, key)?.map_or(Ok(default), |text| {
        text.parse::<u32>().map_err(|_| -libc::EINVAL)
    })?;
    (value != 0).then_some(value).ok_or(-libc::EINVAL)
}

fn boolean(info: Option<&sys::spa_dict>, key: &[u8], default: bool) -> Result<bool, i32> {
    match optional_info(info, key)? {
        None => Ok(default),
        Some("true" | "1" | "yes" | "on") => Ok(true),
        Some("false" | "0" | "no" | "off") => Ok(false),
        Some(_) => Err(-libc::EINVAL),
    }
}

fn rate(info: Option<&sys::spa_dict>) -> Result<Rate, i32> {
    let text = optional_info(info, KEY_RATE)?.unwrap_or("2000/1");
    let (num, denom) = text.split_once('/').ok_or(-libc::EINVAL)?;
    Rate::new(
        num.parse::<u32>().map_err(|_| -libc::EINVAL)?,
        denom.parse::<u32>().map_err(|_| -libc::EINVAL)?,
    )
}

fn decode_u8(
    source: &[u8],
    destination: &mut [u8],
    width: usize,
    height: usize,
    destination_stride: usize,
) -> Result<(), i32> {
    let pixels = width.checked_mul(height).ok_or(-libc::EOVERFLOW)?;
    let destination_span = destination_stride
        .checked_mul(height.saturating_sub(1))
        .and_then(|prefix| prefix.checked_add(width))
        .ok_or(-libc::EOVERFLOW)?;
    if source.len() < pixels
        || destination.len() < destination_span
        || destination_stride < width
        || !pixels.is_multiple_of(64)
    {
        return Err(-libc::EINVAL);
    }
    let half = pixels / 2;
    for index in 0..half {
        destination[index / width * destination_stride + index % width] = source[index * 2];
    }
    for group in 0..(pixels / 64) {
        let reverse_group = pixels - 64 - group * 64;
        for index in 0..32 {
            let output = half + group * 32 + index;
            destination[output / width * destination_stride + output % width] =
                source[reverse_group + index * 2 + 1];
        }
    }
    Ok(())
}

fn decode_u16(
    source: &[u8],
    destination: &mut [u16],
    width: usize,
    height: usize,
    destination_stride: usize,
) -> Result<(), i32> {
    let pixels = width.checked_mul(height).ok_or(-libc::EOVERFLOW)?;
    let destination_span = destination_stride
        .checked_mul(height.saturating_sub(1))
        .and_then(|prefix| prefix.checked_add(width))
        .ok_or(-libc::EOVERFLOW)?;
    if source.len() < pixels.checked_mul(2).ok_or(-libc::EOVERFLOW)?
        || destination.len() < destination_span
        || destination_stride < width
        || !pixels.is_multiple_of(64)
    {
        return Err(-libc::EINVAL);
    }
    let word = |index: usize| {
        let offset = index * 2;
        u16::from_le_bytes([source[offset], source[offset + 1]])
    };
    let half = pixels / 2;
    for index in 0..half {
        destination[index / width * destination_stride + index % width] = word(index * 2);
    }
    for group in 0..(pixels / 64) {
        let reverse_group = pixels - 64 - group * 64;
        for index in 0..32 {
            let output = half + group * 32 + index;
            destination[output / width * destination_stride + output % width] =
                word(reverse_group + index * 2 + 1);
        }
    }
    Ok(())
}

struct HermesDecoderNode {
    ports: Vec<Port>,
    frames: usize,
    counters: usize,
    width: usize,
    height: usize,
    bits: u32,
}

impl Node for HermesDecoderNode {
    fn new(info: Option<&sys::spa_dict>) -> Result<Self, i32> {
        let frames = positive_u32(info, KEY_FRAMES, 50)?;
        let counters = positive_u32(info, KEY_COUNTERS, 1)?;
        let bits = positive_u32(info, KEY_BITS, 8)?;
        let half_array = boolean(info, KEY_HALF, false)?;
        if counters > 3 || (half_array && counters != 1) {
            return Err(-libc::EINVAL);
        }
        let width = if half_array { 32 } else { 64 };
        let height = 32;
        let pixels = width * height;
        let bytes_per_pixel = match bits {
            8 => 1,
            16 => 2,
            _ => return Err(-libc::EINVAL),
        };
        let rate = rate(info)?;
        let input = Format::ndarray(
            sys::SPA_ELEMENT_TYPE_U8,
            HERMES_RAW_BATCH_SCHEMA,
            [frames, counters, pixels * bytes_per_pixel],
            Some(rate),
        )?;
        let output = Format::ndarray(
            if bits == 8 {
                sys::SPA_ELEMENT_TYPE_U8
            } else {
                sys::SPA_ELEMENT_TYPE_U16_LE
            },
            HERMES_DECODED_BATCH_SCHEMA,
            [frames, counters, height, width],
            Some(rate),
        )?;
        Ok(Self {
            ports: vec![
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_INPUT,
                        id: 0,
                    },
                    true,
                    false,
                    [FormatConstraint::exact(input)],
                ),
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_OUTPUT,
                        id: 0,
                    },
                    true,
                    false,
                    [FormatConstraint::exact(output)],
                ),
            ],
            frames: frames as usize,
            counters: counters as usize,
            width: width as usize,
            height: height as usize,
            bits,
        })
    }

    fn ports(&self) -> &[Port] {
        &self.ports
    }

    fn ports_mut(&mut self) -> &mut [Port] {
        &mut self.ports
    }

    fn process(&mut self) -> Result<i32, i32> {
        let (inputs, outputs) = self.ports.split_at_mut(OUTPUT);
        let input = &mut inputs[INPUT];
        let output = &mut outputs[0];
        if output.output_pending()? {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        }
        let Some((_input_id, input_buffer)) = input.input_buffer()? else {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let Some((output_id, output_buffer)) = output.reserve_output()? else {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        };
        let input_format = input.format().ok_or(-libc::EIO)?;
        let output_format = output.format().ok_or(-libc::EIO)?;
        let planes = self
            .frames
            .checked_mul(self.counters)
            .ok_or(-libc::EOVERFLOW)?;
        let pixels = self
            .width
            .checked_mul(self.height)
            .ok_or(-libc::EOVERFLOW)?;
        let result = (|| unsafe {
            let source = InputFrame::new(input_buffer, input_format)?;
            let header = source.header();
            let mut destination = OutputFrame::new(output_buffer, output_format)?;
            if source.lines() != planes || destination.lines() != planes * self.height {
                return Err(-libc::EINVAL);
            }
            if self.bits == 8 {
                let source_stride = source.stride();
                let destination_stride = destination.stride();
                let source_bytes = source.bytes();
                let destination_bytes = destination.bytes_mut();
                for plane in 0..planes {
                    let source_start = plane * source_stride;
                    let destination_start = plane * self.height * destination_stride;
                    decode_u8(
                        &source_bytes[source_start..source_start + pixels],
                        &mut destination_bytes[destination_start..],
                        self.width,
                        self.height,
                        destination_stride,
                    )?;
                }
            } else {
                let source_stride = source.stride();
                let source_bytes = source.bytes();
                let (destination_words, destination_stride) = destination.u16_mut()?;
                for plane in 0..planes {
                    let source_start = plane * source_stride;
                    let destination_start = plane * self.height * destination_stride;
                    decode_u16(
                        &source_bytes[source_start..source_start + pixels * 2],
                        &mut destination_words[destination_start..],
                        self.width,
                        self.height,
                        destination_stride,
                    )?;
                }
            }
            destination.set_properties(source.properties())?;
            destination.set_header(header);
            destination.commit();
            Ok::<(), i32>(())
        })();
        if let Err(error) = result {
            output.cancel_output(output_id)?;
            input.reject_input(error)?;
            return Err(error);
        }
        input.consume_input()?;
        output.publish_output(output_id)?;
        Ok(sys::SPA_STATUS_HAVE_DATA as i32)
    }
}

/// Enumerates the HERMES decoder SPA factory.
///
/// # Safety
///
/// `factory` and `index` must be writable pointers supplied by a SPA host.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn spa_handle_factory_enum(
    factory: *mut *const sys::spa_handle_factory,
    index: *mut u32,
) -> i32 {
    if factory.is_null() || index.is_null() {
        return -libc::EINVAL;
    }
    if unsafe { *index } != 0 {
        return 0;
    }
    unsafe {
        ptr::write(factory, FACTORY.as_ptr());
        *index = 1;
    }
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    fn encode_u8(decoded: &[u8]) -> Vec<u8> {
        let pixels = decoded.len();
        let mut carrier = vec![0; pixels];
        let half = pixels / 2;
        for index in 0..half {
            carrier[index * 2] = decoded[index];
        }
        for group in 0..(pixels / 64) {
            let reverse_group = pixels - 64 - group * 64;
            for index in 0..32 {
                carrier[reverse_group + index * 2 + 1] = decoded[half + group * 32 + index];
            }
        }
        carrier
    }

    fn encode_u16(decoded: &[u16]) -> Vec<u8> {
        let pixels = decoded.len();
        let mut words = vec![0_u16; pixels];
        let half = pixels / 2;
        for index in 0..half {
            words[index * 2] = decoded[index];
        }
        for group in 0..(pixels / 64) {
            let reverse_group = pixels - 64 - group * 64;
            for index in 0..32 {
                words[reverse_group + index * 2 + 1] = decoded[half + group * 32 + index];
            }
        }
        words.into_iter().flat_map(u16::to_le_bytes).collect()
    }

    #[test]
    fn u8_permutation_matches_every_pixel() {
        let decoded: Vec<u8> = (0..2048).map(|index| (index % 251) as u8).collect();
        let carrier = encode_u8(&decoded);
        let mut actual = vec![0; decoded.len()];
        decode_u8(&carrier, &mut actual, 64, 32, 64).unwrap();
        assert_eq!(actual, decoded);
    }

    #[test]
    fn u16_permutation_matches_every_pixel() {
        let decoded: Vec<u16> = (0..2048).map(|index| index as u16 + 1000).collect();
        let carrier = encode_u16(&decoded);
        let mut actual = vec![0; decoded.len()];
        decode_u16(&carrier, &mut actual, 64, 32, 64).unwrap();
        assert_eq!(actual, decoded);
    }

    #[test]
    fn decoder_rejects_truncated_or_structurally_invalid_planes() {
        let mut output = vec![0_u8; 2048];
        let invalid_source = [0_u8; 65];
        let mut invalid_output = [0_u8; 65];
        assert_eq!(
            decode_u8(&vec![0; 2047], &mut output, 64, 32, 64),
            Err(-libc::EINVAL)
        );
        assert_eq!(
            decode_u8(&invalid_source, &mut invalid_output, 65, 1, 65),
            Err(-libc::EINVAL)
        );
    }
}

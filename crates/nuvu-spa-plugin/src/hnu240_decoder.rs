//! HNü240 Camera Link Full carrier decoding.
//!
//! The byte mapping follows Tables 3 and 4 of Beaulieu et al.,
//! "Electron multiplying CCDs for sensitive wavefront sensing at 3k frames
//! per second" (SPIE 2022). The transport contains ten leading pipeline rows
//! and six leading samples per tap. The final carrier row contains one
//! overscan line from every tap and is discarded.

use pipewireao_spa_node::{
    Factory, Format, FormatConstraint, InputFrame, Node, OutputFrame, Port, PortRef, sys,
};

use crate::config::{parse_rate, required_info};

/// Factory name of the complete-frame HNü240 carrier decoder.
pub const HNU240_DECODER_FACTORY_NAME: &str = "api.hnu240.decoder";

/// Exact carrier profile accepted by the decoder factory.
pub const HNU240_CL_FULL_PROFILE: &str = "hnu240-cl-full-8x8-v1";

pub(crate) const RAW_WIDTH: usize = 1408;
pub(crate) const OUTPUT_WIDTH: usize = 240;
pub(crate) const OUTPUT_HEIGHT: usize = 240;

const PIPELINE_ROWS: usize = 10;
const ACTIVE_LINES_PER_TAP: usize = 120;
const CARRIER_LINES_PER_TAP: usize = ACTIVE_LINES_PER_TAP + 1;
pub(crate) const RAW_HEIGHT: usize = PIPELINE_ROWS + CARRIER_LINES_PER_TAP;
const GROUP_BYTES: usize = 64;
const GROUPS_PER_ROW: usize = RAW_WIDTH / GROUP_BYTES;
const LEADING_GROUPS: usize = 2;
const PIXELS_PER_GROUP_PER_TAP: usize = 3;
const TAP_WIDTH: usize = 60;

const KEY_RATE: &[u8] = b"api.hnu240.frame-rate\0";
const KEY_PROFILE: &[u8] = b"api.hnu240.transport-profile\0";
const INPUT: usize = 0;
const OUTPUT: usize = 1;

pub(crate) static FACTORY: Factory = Factory::new::<Hnu240DecoderNode>(b"api.hnu240.decoder\0");

// Byte offsets within one 64-byte carrier group, indexed by the tap labels in
// the paper and then by the three consecutive samples from that tap.
const PIXEL_BYTES: [[(usize, usize); PIXELS_PER_GROUP_PER_TAP]; 8] = [
    [(0, 1), (18, 24), (41, 42)],
    [(2, 8), (25, 26), (48, 49)],
    [(9, 10), (32, 33), (50, 56)],
    [(16, 17), (34, 40), (57, 58)],
    [(3, 4), (21, 27), (44, 45)],
    [(5, 11), (28, 29), (51, 52)],
    [(12, 13), (35, 36), (53, 59)],
    [(19, 20), (37, 43), (60, 61)],
];

#[derive(Clone, Copy)]
struct TapPlacement {
    x: isize,
    dx: isize,
    y: isize,
    dy: isize,
}

// The CCD220 view in the paper is rotated clockwise into the conventional
// active-pixel image. Rows are reconstructed from the outside toward the
// centre. The carrier's final per-tap line is overscan and is never mapped.
const TAP_PLACEMENTS: [TapPlacement; 8] = [
    TapPlacement {
        x: 179,
        dx: -1,
        y: 0,
        dy: 1,
    }, // T0
    TapPlacement {
        x: 180,
        dx: 1,
        y: 0,
        dy: 1,
    }, // T1
    TapPlacement {
        x: 180,
        dx: 1,
        y: 239,
        dy: -1,
    }, // T2
    TapPlacement {
        x: 179,
        dx: -1,
        y: 239,
        dy: -1,
    }, // T3
    TapPlacement {
        x: 60,
        dx: 1,
        y: 239,
        dy: -1,
    }, // T4
    TapPlacement {
        x: 59,
        dx: -1,
        y: 239,
        dy: -1,
    }, // T5
    TapPlacement {
        x: 59,
        dx: -1,
        y: 0,
        dy: 1,
    }, // T6
    TapPlacement {
        x: 60,
        dx: 1,
        y: 0,
        dy: 1,
    }, // T7
];

/// Decodes one complete HNü240 carrier frame into detector pixels.
fn decode_frame(
    source: &[u8],
    source_stride: usize,
    destination: &mut [u16],
    destination_stride: usize,
) -> Result<(), i32> {
    if source_stride < RAW_WIDTH
        || destination_stride < OUTPUT_WIDTH
        || source.len()
            < source_stride
                .checked_mul(RAW_HEIGHT)
                .ok_or(-libc::EOVERFLOW)?
        || destination.len()
            < destination_stride
                .checked_mul(OUTPUT_HEIGHT)
                .ok_or(-libc::EOVERFLOW)?
    {
        return Err(-libc::EINVAL);
    }

    for carrier_row in PIPELINE_ROWS..PIPELINE_ROWS + ACTIVE_LINES_PER_TAP {
        let detector_row = carrier_row - PIPELINE_ROWS;
        let row_start = carrier_row * source_stride;
        for group in LEADING_GROUPS..GROUPS_PER_ROW {
            let group_start = row_start + group * GROUP_BYTES;
            let first_sample = (group - LEADING_GROUPS) * PIXELS_PER_GROUP_PER_TAP;
            for (tap, samples) in PIXEL_BYTES.iter().enumerate() {
                let placement = TAP_PLACEMENTS[tap];
                let y = placement.y + placement.dy * detector_row as isize;
                debug_assert!((0..OUTPUT_HEIGHT as isize).contains(&y));
                for (sample_in_group, &(low, high)) in samples.iter().enumerate() {
                    let sample = first_sample + sample_in_group;
                    debug_assert!(sample < TAP_WIDTH);
                    let x = placement.x + placement.dx * sample as isize;
                    debug_assert!((0..OUTPUT_WIDTH as isize).contains(&x));
                    destination[y as usize * destination_stride + x as usize] =
                        u16::from_le_bytes([source[group_start + low], source[group_start + high]]);
                }
            }
        }
    }
    Ok(())
}

struct Hnu240DecoderNode {
    ports: Vec<Port>,
}

impl Node for Hnu240DecoderNode {
    fn new(info: Option<&sys::spa_dict>) -> Result<Self, i32> {
        let rate = parse_rate(required_info(info, KEY_RATE)?)?;
        if required_info(info, KEY_PROFILE)? != HNU240_CL_FULL_PROFILE {
            return Err(-libc::EINVAL);
        }
        let input_format = Format::gray8(RAW_WIDTH as u32, RAW_HEIGHT as u32, rate)?;
        let output_format = Format::gray16_le(OUTPUT_WIDTH as u32, OUTPUT_HEIGHT as u32, rate)?;
        Ok(Self {
            ports: vec![
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_INPUT,
                        id: 0,
                    },
                    true,
                    false,
                    [FormatConstraint::exact(input_format)],
                ),
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_OUTPUT,
                        id: 0,
                    },
                    true,
                    false,
                    [FormatConstraint::exact(output_format)],
                ),
            ],
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
        let result = (|| unsafe {
            let source = InputFrame::new(input_buffer, input_format)?;
            let header = source.header();
            let (carrier, carrier_stride) = source.u8();
            let mut destination = OutputFrame::new(output_buffer, output_format)?;
            let (pixels, pixel_stride) = destination.u16_mut()?;
            decode_frame(carrier, carrier_stride, pixels, pixel_stride)?;
            destination.set_metadata(source.metadata())?;
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

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_value(tap: usize, row: usize, sample: usize) -> u16 {
        u16::try_from(1 + tap * 7_500 + row * TAP_WIDTH + sample).unwrap()
    }

    #[test]
    fn published_carrier_mapping_reconstructs_active_pixels_and_discards_overscan() {
        let mut carrier = vec![0xa5; RAW_WIDTH * RAW_HEIGHT];
        let mut expected = vec![0_u16; OUTPUT_WIDTH * OUTPUT_HEIGHT];
        let mut overscan = Vec::with_capacity(8 * TAP_WIDTH);
        for carrier_row in PIPELINE_ROWS..RAW_HEIGHT {
            let detector_row = carrier_row - PIPELINE_ROWS;
            for group in LEADING_GROUPS..GROUPS_PER_ROW {
                let group_start = carrier_row * RAW_WIDTH + group * GROUP_BYTES;
                let first_sample = (group - LEADING_GROUPS) * PIXELS_PER_GROUP_PER_TAP;
                for (tap, samples) in PIXEL_BYTES.iter().enumerate() {
                    let placement = TAP_PLACEMENTS[tap];
                    let y = (placement.y + placement.dy * detector_row as isize) as usize;
                    for (sample_in_group, &(low, high)) in samples.iter().enumerate() {
                        let sample = first_sample + sample_in_group;
                        let value = sample_value(tap, detector_row, sample);
                        let bytes = value.to_le_bytes();
                        carrier[group_start + low] = bytes[0];
                        carrier[group_start + high] = bytes[1];
                        if detector_row == ACTIVE_LINES_PER_TAP {
                            overscan.push(value);
                            continue;
                        }
                        let x = (placement.x + placement.dx * sample as isize) as usize;
                        expected[y * OUTPUT_WIDTH + x] = value;
                    }
                }
            }
        }

        let mut decoded = vec![0_u16; OUTPUT_WIDTH * OUTPUT_HEIGHT];
        decode_frame(&carrier, RAW_WIDTH, &mut decoded, OUTPUT_WIDTH).unwrap();
        assert_eq!(decoded, expected);
        assert!(decoded.iter().all(|&pixel| pixel != 0));
        assert!(overscan.iter().all(|value| !decoded.contains(value)));
    }

    #[test]
    fn decoder_rejects_short_or_narrow_storage() {
        let carrier = vec![0_u8; RAW_WIDTH * RAW_HEIGHT];
        let mut decoded = vec![0_u16; OUTPUT_WIDTH * OUTPUT_HEIGHT];
        assert_eq!(
            decode_frame(
                &carrier[..carrier.len() - 1],
                RAW_WIDTH,
                &mut decoded,
                OUTPUT_WIDTH
            ),
            Err(-libc::EINVAL)
        );
        assert_eq!(
            decode_frame(&carrier, RAW_WIDTH - 1, &mut decoded, OUTPUT_WIDTH),
            Err(-libc::EINVAL)
        );
        assert_eq!(
            decode_frame(&carrier, RAW_WIDTH, &mut decoded, OUTPUT_WIDTH - 1),
            Err(-libc::EINVAL)
        );
    }
}

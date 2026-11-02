//! Exact packed raw-video to ndarray structural view.

use pipewireao_spa_node::{
    Factory, Format, FormatConstraint, InputFrame, Node, OutputFrame, Port, PortRef, Rate,
    forward_frame, shares_data, sys,
};

use crate::config::{parse_rate, parse_size, required_info};

/// Factory name of the packed raw-video to ndarray adapter.
pub const VIDEO_VIEW_FACTORY_NAME: &str = "api.ndarray.video-view";

const KEY_SIZE: &[u8] = b"api.ndarray.frame-size\0";
const KEY_RATE: &[u8] = b"api.ndarray.frame-rate\0";
const KEY_SCHEMA: &[u8] = b"api.ndarray.schema\0";
const KEY_VIDEO_FORMAT: &[u8] = b"api.ndarray.video-format\0";
const INPUT: usize = 0;
const OUTPUT: usize = 1;

pub(crate) static FACTORY: Factory = Factory::new::<VideoViewNode>(b"api.ndarray.video-view\0");

struct VideoViewNode {
    ports: Vec<Port>,
}

impl VideoViewNode {
    fn copy_frame(
        source: &InputFrame<'_>,
        output_format: &Format,
        destination: &mut OutputFrame<'_>,
    ) -> Result<(), i32> {
        let line_bytes = output_format.packed_stride()?;
        let lines = output_format.line_count()?;
        if source.lines() != lines
            || source.stride() < line_bytes
            || destination.lines() != lines
            || destination.stride() < line_bytes
        {
            return Err(-libc::EINVAL);
        }
        let source_stride = source.stride();
        let destination_stride = destination.stride();
        let source_bytes = source.bytes();
        let destination_bytes = destination.bytes_mut();
        for line in 0..lines {
            destination_bytes[line * destination_stride..line * destination_stride + line_bytes]
                .copy_from_slice(
                    &source_bytes[line * source_stride..line * source_stride + line_bytes],
                );
        }
        Ok(())
    }
}

impl Node for VideoViewNode {
    fn new(info: Option<&sys::spa_dict>) -> Result<Self, i32> {
        let (width, height) = parse_size(required_info(info, KEY_SIZE)?)?;
        let rate: Rate = parse_rate(required_info(info, KEY_RATE)?)?;
        let schema: Box<str> = required_info(info, KEY_SCHEMA)?.into();
        if schema.is_empty() {
            return Err(-libc::EINVAL);
        }
        let (input_format, element_type) = match required_info(info, KEY_VIDEO_FORMAT)? {
            "GRAY8" => (
                Format::gray8(width, height, rate)?,
                sys::SPA_ELEMENT_TYPE_U8,
            ),
            "GRAY16_LE" => (
                Format::gray16_le(width, height, rate)?,
                sys::SPA_ELEMENT_TYPE_U16_LE,
            ),
            _ => return Err(-libc::EINVAL),
        };
        let output_format = Format::ndarray_format(
            element_type,
            Some(schema),
            [height, width],
            sys::SPA_NDARRAY_LAYOUT_ROW_MAJOR,
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

    fn format_changed(&mut self, _port: usize) -> Result<(), i32> {
        let compatible = match (self.ports[INPUT].format(), self.ports[OUTPUT].format()) {
            (Some(input), Some(output)) => {
                input.same_shape_and_rate(output)
                    && input.packed_bytes()? == output.packed_bytes()?
                    && input.packed_stride()? == output.packed_stride()?
            }
            _ => false,
        };
        self.ports[INPUT].set_can_allocate_buffers(compatible);
        self.ports[OUTPUT].set_can_allocate_buffers(compatible);
        Ok(())
    }

    fn process(&mut self) -> Result<i32, i32> {
        if self.ports[OUTPUT].output_pending()? {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        }
        let Some((input_id, input_buffer)) = self.ports[INPUT].input_buffer()? else {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let input_format = self.ports[INPUT].format().ok_or(-libc::EIO)?;
        let output_format = self.ports[OUTPUT].format().cloned().ok_or(-libc::EIO)?;
        let source = unsafe { InputFrame::new(input_buffer, input_format)? };
        let header = source.header();
        let shared = self.ports[OUTPUT]
            .registered_buffer(input_id)
            .is_some_and(|output| unsafe { shares_data(input_buffer, output) });
        let Some((output_id, output_buffer)) = (if shared {
            self.ports[OUTPUT]
                .reserve_output_id(input_id)?
                .map(|buffer| (input_id, buffer))
        } else {
            self.ports[OUTPUT].reserve_output()?
        }) else {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        };
        let result = (|| unsafe {
            if !forward_frame(input_buffer, output_buffer, &output_format, header)? {
                let mut destination = OutputFrame::new(output_buffer, &output_format)?;
                Self::copy_frame(&source, &output_format, &mut destination)?;
                destination.set_properties(source.properties())?;
                destination.set_header(header);
                destination.commit();
            }
            Ok::<(), i32>(())
        })();
        if let Err(error) = result {
            self.ports[OUTPUT].cancel_output(output_id)?;
            return Err(error);
        }
        self.ports[INPUT].consume_input()?;
        self.ports[OUTPUT].publish_output(output_id)?;
        Ok(sys::SPA_STATUS_HAVE_DATA as i32)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepted_video_vocabulary_is_explicit() {
        assert_eq!(VIDEO_VIEW_FACTORY_NAME, "api.ndarray.video-view");
        assert_ne!(sys::SPA_ELEMENT_TYPE_U8, sys::SPA_ELEMENT_TYPE_U16_LE);
    }
}

//! Assembly of rank-two row-block ndarrays into complete frames.

use pipewireao_spa_node::{
    Factory, Format, FormatConstraint, Header, InputFrame, Node, OutputFrame, Port, PortRef, Rate,
    RetainedMetadata, forward_frame, shares_data, sys,
};

use crate::config::{
    optional_nonempty, parse_positive_usize, parse_rate, parse_size, required_info,
};

/// Factory name of the ndarray frame assembler.
pub const FRAME_ASSEMBLY_FACTORY_NAME: &str = "api.ndarray.frame-assembly";

const KEY_SIZE: &[u8] = b"api.ndarray.frame-size\0";
const KEY_RATE: &[u8] = b"api.ndarray.frame-rate\0";
const KEY_BLOCK_SCHEMA: &[u8] = b"api.ndarray.row-block-schema\0";
const KEY_FRAME_SCHEMA: &[u8] = b"api.ndarray.frame-schema\0";
const KEY_ELEMENT_TYPE: &[u8] = b"api.ndarray.element-type\0";
const KEY_LAYOUT: &[u8] = b"api.ndarray.layout\0";
const KEY_BLOCK_ROWS: &[u8] = b"api.ndarray.row-block-rows\0";
const INPUT: usize = 0;
const OUTPUT: usize = 1;

pub(crate) static FACTORY: Factory =
    Factory::new::<FrameAssemblyNode>(b"api.ndarray.frame-assembly\0");

struct FrameAssemblyNode {
    ports: Vec<Port>,
    width: usize,
    height: usize,
    block_rows: usize,
    element_size: usize,
    layout: u32,
    frame: Box<[u8]>,
    active_seq: Option<u64>,
    next_row: usize,
    complete_header: Option<Header>,
    metadata: RetainedMetadata,
    active_flags: u32,
    next_flags: u32,
}

impl FrameAssemblyNode {
    fn abandon_frame(&mut self) {
        self.active_seq = None;
        self.next_row = 0;
        self.active_flags = 0;
        self.metadata.clear();
        self.next_flags |= sys::SPA_META_HEADER_FLAG_DISCONT;
    }

    fn copy_block(&mut self, source: &InputFrame<'_>, start_row: usize) -> Result<(), i32> {
        if self.layout == sys::SPA_NDARRAY_LAYOUT_ROW_MAJOR {
            let line_bytes = self
                .width
                .checked_mul(self.element_size)
                .ok_or(-libc::EOVERFLOW)?;
            if source.lines() != self.block_rows || source.stride() < line_bytes {
                return Err(-libc::EINVAL);
            }
            for row in 0..self.block_rows {
                let source_start = row * source.stride();
                let destination_start = (start_row + row) * line_bytes;
                self.frame[destination_start..destination_start + line_bytes]
                    .copy_from_slice(&source.bytes()[source_start..source_start + line_bytes]);
            }
        } else {
            let block_bytes = self
                .block_rows
                .checked_mul(self.element_size)
                .ok_or(-libc::EOVERFLOW)?;
            let column_bytes = self
                .height
                .checked_mul(self.element_size)
                .ok_or(-libc::EOVERFLOW)?;
            let row_offset = start_row
                .checked_mul(self.element_size)
                .ok_or(-libc::EOVERFLOW)?;
            if source.lines() != self.width || source.stride() < block_bytes {
                return Err(-libc::EINVAL);
            }
            for column in 0..self.width {
                let source_start = column * source.stride();
                let destination_start = column * column_bytes + row_offset;
                self.frame[destination_start..destination_start + block_bytes]
                    .copy_from_slice(&source.bytes()[source_start..source_start + block_bytes]);
            }
        }
        Ok(())
    }

    fn copy_frame(
        frame: &[u8],
        output_format: &Format,
        destination: &mut OutputFrame<'_>,
    ) -> Result<(), i32> {
        let line_bytes = output_format.packed_stride()?;
        let lines = output_format.line_count()?;
        if destination.lines() != lines
            || destination.stride() < line_bytes
            || frame.len() != line_bytes.checked_mul(lines).ok_or(-libc::EOVERFLOW)?
        {
            return Err(-libc::EINVAL);
        }
        let stride = destination.stride();
        let bytes = destination.bytes_mut();
        for line in 0..lines {
            bytes[line * stride..line * stride + line_bytes]
                .copy_from_slice(&frame[line * line_bytes..(line + 1) * line_bytes]);
        }
        Ok(())
    }

    fn copy_input(
        source: &InputFrame<'_>,
        format: &Format,
        destination: &mut OutputFrame<'_>,
    ) -> Result<(), i32> {
        let line_bytes = format.packed_stride()?;
        let lines = format.line_count()?;
        if source.lines() != lines
            || source.stride() < line_bytes
            || destination.lines() != lines
            || destination.stride() < line_bytes
        {
            return Err(-libc::EINVAL);
        }
        let destination_stride = destination.stride();
        let destination_bytes = destination.bytes_mut();
        for line in 0..lines {
            destination_bytes[line * destination_stride..line * destination_stride + line_bytes]
                .copy_from_slice(
                    &source.bytes()[line * source.stride()..line * source.stride() + line_bytes],
                );
        }
        Ok(())
    }

    fn publish_frame(
        &mut self,
        input_id: u32,
        input_buffer: *mut sys::spa_buffer,
        source: &InputFrame<'_>,
        mut header: Header,
    ) -> Result<i32, i32> {
        header.flags |= self.next_flags;
        self.next_flags = 0;

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
        let output_format = self.ports[OUTPUT].format().ok_or(-libc::EIO)?;
        let result = (|| unsafe {
            if !forward_frame(input_buffer, output_buffer, output_format, Some(header))? {
                let mut destination = OutputFrame::new(output_buffer, output_format)?;
                Self::copy_input(source, output_format, &mut destination)?;
                destination.set_metadata(source.metadata())?;
                destination.set_header(Some(header));
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

    fn publish_complete(&mut self) -> Result<i32, i32> {
        let Some(mut header) = self.complete_header else {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let output = &mut self.ports[OUTPUT];
        if output.output_pending()? {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        }
        let Some((output_id, output_buffer)) = output.reserve_output()? else {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        };
        let output_format = output.format().ok_or(-libc::EIO)?;
        let result = (|| unsafe {
            let mut destination = OutputFrame::new(output_buffer, output_format)?;
            Self::copy_frame(&self.frame, output_format, &mut destination)?;
            header.offset = 0;
            header.flags |= sys::SPA_META_HEADER_FLAG_MARKER;
            self.metadata.copy_to(&mut destination)?;
            destination.set_header(Some(header));
            destination.commit();
            Ok::<(), i32>(())
        })();
        if let Err(error) = result {
            output.cancel_output(output_id)?;
            return Err(error);
        }
        output.publish_output(output_id)?;
        self.complete_header = None;
        Ok(sys::SPA_STATUS_HAVE_DATA as i32)
    }
}

impl Node for FrameAssemblyNode {
    fn new(info: Option<&sys::spa_dict>) -> Result<Self, i32> {
        let (width, height) = parse_size(required_info(info, KEY_SIZE)?)?;
        let frame_rate = optional_nonempty(info, KEY_RATE)?
            .map(parse_rate)
            .transpose()?;
        let block_schema = optional_nonempty(info, KEY_BLOCK_SCHEMA)?.map(Into::into);
        let frame_schema = optional_nonempty(info, KEY_FRAME_SCHEMA)?.map(Into::into);
        let element_type = parse_element_type(required_info(info, KEY_ELEMENT_TYPE)?)?;
        let layout = parse_layout(required_info(info, KEY_LAYOUT)?)?;
        let block_rows = parse_positive_usize(required_info(info, KEY_BLOCK_ROWS)?)?;
        if block_rows >= height as usize || !(height as usize).is_multiple_of(block_rows) {
            return Err(-libc::EINVAL);
        }
        let blocks = height as usize / block_rows;
        let block_rate = frame_rate
            .map(|frame_rate| {
                Rate::new(
                    frame_rate
                        .num
                        .checked_mul(u32::try_from(blocks).map_err(|_| -libc::EOVERFLOW)?)
                        .ok_or(-libc::EOVERFLOW)?,
                    frame_rate.denom,
                )
            })
            .transpose()?;
        let input_format = Format::ndarray_format(
            element_type,
            block_schema,
            [
                u32::try_from(block_rows).map_err(|_| -libc::EOVERFLOW)?,
                width,
            ],
            layout,
            block_rate,
        )?;
        let output_format = Format::ndarray_format(
            element_type,
            frame_schema,
            [height, width],
            layout,
            frame_rate,
        )?;
        let frame_bytes = output_format.packed_bytes()?;
        let element_size = output_format.element_size()?;
        Ok(Self {
            ports: vec![
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_INPUT,
                        id: 0,
                    },
                    true,
                    false,
                    [
                        FormatConstraint::exact(input_format),
                        FormatConstraint::exact(output_format.clone()),
                    ],
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
            width: width as usize,
            height: height as usize,
            block_rows,
            element_size,
            layout,
            frame: vec![0; frame_bytes].into_boxed_slice(),
            active_seq: None,
            next_row: 0,
            complete_header: None,
            metadata: RetainedMetadata::default(),
            active_flags: 0,
            next_flags: 0,
        })
    }

    fn ports(&self) -> &[Port] {
        &self.ports
    }

    fn ports_mut(&mut self) -> &mut [Port] {
        &mut self.ports
    }

    fn format_changed(&mut self, _port: usize) -> Result<(), i32> {
        let passthrough = matches!(
            (
                self.ports[INPUT].format(),
                self.ports[OUTPUT].format(),
            ),
            (Some(input), Some(output)) if input == output
        );
        self.ports[INPUT].set_can_allocate_buffers(passthrough);
        self.ports[OUTPUT].set_can_allocate_buffers(passthrough);
        Ok(())
    }

    fn pause(&mut self) {
        self.active_seq = None;
        self.next_row = 0;
        self.complete_header = None;
        self.active_flags = 0;
        self.metadata.clear();
        self.next_flags |= sys::SPA_META_HEADER_FLAG_DISCONT;
    }

    fn process(&mut self) -> Result<i32, i32> {
        if self.complete_header.is_some() {
            return self.publish_complete();
        }
        if self.ports[OUTPUT].output_pending()? {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        }
        let Some((input_id, input_buffer)) = self.ports[INPUT].input_buffer()? else {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let Some(input_format) = self.ports[INPUT].format() else {
            self.ports[INPUT].consume_input()?;
            self.abandon_frame();
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let source = match unsafe { InputFrame::new(input_buffer, input_format) } {
            Ok(source) => source,
            Err(_) => {
                self.ports[INPUT].consume_input()?;
                self.abandon_frame();
                return Ok(sys::SPA_STATUS_NEED_DATA as i32);
            }
        };
        let Some(header) = source.header() else {
            self.ports[INPUT].consume_input()?;
            self.abandon_frame();
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        if self.ports[OUTPUT].format() == Some(input_format) {
            return self.publish_frame(input_id, input_buffer, &source, header);
        }
        let start_row = header.offset as usize;
        let Some(end_row) = start_row.checked_add(self.block_rows) else {
            self.ports[INPUT].consume_input()?;
            self.abandon_frame();
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let marker = header.flags & sys::SPA_META_HEADER_FLAG_MARKER != 0;
        if self.active_seq != Some(header.seq) {
            if self.active_seq.is_some() {
                self.abandon_frame();
            }
            if start_row != 0 {
                self.ports[INPUT].consume_input()?;
                self.abandon_frame();
                return Ok(sys::SPA_STATUS_NEED_DATA as i32);
            }
            self.metadata.capture(source.metadata())?;
            self.active_seq = Some(header.seq);
            self.next_row = 0;
            self.active_flags =
                self.next_flags | (header.flags & !sys::SPA_META_HEADER_FLAG_MARKER);
            self.next_flags = 0;
        } else {
            if !self.metadata.matches(source.metadata()) {
                self.ports[INPUT].consume_input()?;
                self.abandon_frame();
                return Ok(sys::SPA_STATUS_NEED_DATA as i32);
            }
            self.active_flags |= header.flags & !sys::SPA_META_HEADER_FLAG_MARKER;
        }
        if start_row != self.next_row || end_row > self.height || marker != (end_row == self.height)
        {
            self.ports[INPUT].consume_input()?;
            self.abandon_frame();
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        }

        if self.copy_block(&source, start_row).is_err() {
            self.ports[INPUT].consume_input()?;
            self.abandon_frame();
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        }
        self.ports[INPUT].consume_input()?;
        self.next_row = end_row;
        if marker {
            self.complete_header = Some(Header {
                flags: self.active_flags | sys::SPA_META_HEADER_FLAG_MARKER,
                offset: 0,
                ..header
            });
            self.active_seq = None;
            self.next_row = 0;
            self.active_flags = 0;
            return self.publish_complete();
        }
        Ok(sys::SPA_STATUS_NEED_DATA as i32)
    }
}

fn parse_layout(value: &str) -> Result<u32, i32> {
    match value {
        "row-major" => Ok(sys::SPA_NDARRAY_LAYOUT_ROW_MAJOR),
        "column-major" => Ok(sys::SPA_NDARRAY_LAYOUT_COLUMN_MAJOR),
        _ => Err(-libc::EINVAL),
    }
}

fn parse_element_type(value: &str) -> Result<u32, i32> {
    match value {
        "BOOL8" => Ok(sys::SPA_ELEMENT_TYPE_BOOL8),
        "I8" => Ok(sys::SPA_ELEMENT_TYPE_I8),
        "U8" => Ok(sys::SPA_ELEMENT_TYPE_U8),
        "I16_LE" => Ok(sys::SPA_ELEMENT_TYPE_I16_LE),
        "U16_LE" => Ok(sys::SPA_ELEMENT_TYPE_U16_LE),
        "I32_LE" => Ok(sys::SPA_ELEMENT_TYPE_I32_LE),
        "U32_LE" => Ok(sys::SPA_ELEMENT_TYPE_U32_LE),
        "I64_LE" => Ok(sys::SPA_ELEMENT_TYPE_I64_LE),
        "U64_LE" => Ok(sys::SPA_ELEMENT_TYPE_U64_LE),
        "I128_LE" => Ok(sys::SPA_ELEMENT_TYPE_I128_LE),
        "U128_LE" => Ok(sys::SPA_ELEMENT_TYPE_U128_LE),
        "F8_E4M3FN" => Ok(sys::SPA_ELEMENT_TYPE_F8_E4M3FN),
        "F8_E4M3FNUZ" => Ok(sys::SPA_ELEMENT_TYPE_F8_E4M3FNUZ),
        "F8_E5M2" => Ok(sys::SPA_ELEMENT_TYPE_F8_E5M2),
        "F8_E5M2FNUZ" => Ok(sys::SPA_ELEMENT_TYPE_F8_E5M2FNUZ),
        "F16_LE" => Ok(sys::SPA_ELEMENT_TYPE_F16_LE),
        "BF16_LE" => Ok(sys::SPA_ELEMENT_TYPE_BF16_LE),
        "F32_LE" => Ok(sys::SPA_ELEMENT_TYPE_F32_LE),
        "F64_LE" => Ok(sys::SPA_ELEMENT_TYPE_F64_LE),
        "F128_LE" => Ok(sys::SPA_ELEMENT_TYPE_F128_LE),
        "COMPLEX_F16_LE" => Ok(sys::SPA_ELEMENT_TYPE_COMPLEX_F16_LE),
        "COMPLEX_BF16_LE" => Ok(sys::SPA_ELEMENT_TYPE_COMPLEX_BF16_LE),
        "COMPLEX_F32_LE" => Ok(sys::SPA_ELEMENT_TYPE_COMPLEX_F32_LE),
        "COMPLEX_F64_LE" => Ok(sys::SPA_ELEMENT_TYPE_COMPLEX_F64_LE),
        "COMPLEX_F128_LE" => Ok(sys::SPA_ELEMENT_TYPE_COMPLEX_F128_LE),
        _ => Err(-libc::EINVAL),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn construction_vocabulary_is_exact() {
        assert_eq!(
            parse_layout("row-major"),
            Ok(sys::SPA_NDARRAY_LAYOUT_ROW_MAJOR)
        );
        assert_eq!(
            parse_layout("column-major"),
            Ok(sys::SPA_NDARRAY_LAYOUT_COLUMN_MAJOR)
        );
        assert_eq!(parse_layout("ROW_MAJOR"), Err(-libc::EINVAL));
        assert_eq!(
            parse_element_type("COMPLEX_F128_LE"),
            Ok(sys::SPA_ELEMENT_TYPE_COMPLEX_F128_LE)
        );
        assert_eq!(parse_element_type("f32"), Err(-libc::EINVAL));
        assert_eq!(parse_element_type("CUSTOM"), Err(-libc::EINVAL));
    }
}

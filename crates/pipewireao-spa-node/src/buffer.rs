//! Validated views over registered SPA memory buffers.

use std::mem::{align_of, size_of};

use libspa::sys;

use crate::Format;

pub(crate) fn is_cpu_mapped(type_: u32) -> bool {
    matches!(type_, sys::SPA_DATA_MemPtr | sys::SPA_DATA_MemFd)
}

/// Copyable standard SPA header metadata.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Header {
    /// SPA header flags.
    pub flags: u32,
    /// Position within the current graph cycle.
    pub offset: u32,
    /// Presentation timestamp in nanoseconds.
    pub pts: i64,
    /// Decoding timestamp offset from `pts`.
    pub dts_offset: i64,
    /// Stream sequence number or prepared-object identity.
    pub seq: u64,
}

impl From<sys::spa_meta_header> for Header {
    fn from(header: sys::spa_meta_header) -> Self {
        Self {
            flags: header.flags,
            offset: header.offset,
            pts: header.pts,
            dts_offset: header.dts_offset,
            seq: header.seq,
        }
    }
}

impl From<Header> for sys::spa_meta_header {
    fn from(header: Header) -> Self {
        Self {
            flags: header.flags,
            offset: header.offset,
            pts: header.pts,
            dts_offset: header.dts_offset,
            seq: header.seq,
        }
    }
}

/// Read-only validated complete-sample storage.
pub struct InputFrame<'a> {
    bytes: &'a [u8],
    stride: usize,
    lines: usize,
    header: Option<Header>,
    properties: Option<&'a [u8]>,
}

impl<'a> InputFrame<'a> {
    /// Validates one registered SPA input buffer against its negotiated format.
    ///
    /// # Safety
    ///
    /// `buffer` must remain registered and exclusively host-owned for the
    /// returned borrow.
    pub unsafe fn new(buffer: *const sys::spa_buffer, format: &Format) -> Result<Self, i32> {
        let buffer = unsafe { buffer.as_ref() }.ok_or(-libc::EINVAL)?;
        if buffer.n_datas != 1 || buffer.datas.is_null() {
            return Err(-libc::EINVAL);
        }
        let data = unsafe { &*buffer.datas };
        let chunk = unsafe { data.chunk.as_ref() }.ok_or(-libc::EINVAL)?;
        if !is_cpu_mapped(data.type_) || data.data.is_null() || data.maxsize == 0 {
            return Err(-libc::EINVAL);
        }
        let line_bytes = format.packed_stride()?;
        let lines = format.line_count()?;
        let stride = read_stride(chunk.stride, line_bytes)?;
        let span = frame_span(line_bytes, stride, lines)?;
        let offset = chunk.offset as usize % data.maxsize as usize;
        if (chunk.size as usize) < span
            || offset
                .checked_add(span)
                .is_none_or(|end| end > data.maxsize as usize)
        {
            return Err(-libc::EINVAL);
        }
        let bytes = unsafe { std::slice::from_raw_parts(data.data.cast::<u8>().add(offset), span) };
        let header = unsafe { header(buffer).copied().map(Header::from) };
        let properties = unsafe { properties(buffer)? };
        Ok(Self {
            bytes,
            stride,
            lines,
            header,
            properties,
        })
    }

    /// Returns the complete byte span, including inter-line padding.
    pub const fn bytes(&self) -> &[u8] {
        self.bytes
    }

    /// Returns the physical byte distance between storage lines.
    pub const fn stride(&self) -> usize {
        self.stride
    }

    /// Returns the physical storage-line count.
    pub const fn lines(&self) -> usize {
        self.lines
    }

    /// Returns copied header metadata when the buffer carries it.
    pub const fn header(&self) -> Option<Header> {
        self.header
    }

    /// Complete validated Properties Object bytes, borrowed under the input lease.
    pub const fn properties(&self) -> Option<&[u8]> {
        self.properties
    }

    /// Borrows aligned `u16` storage, including line padding before the final line.
    pub fn u16(&self) -> Result<(&[u16], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows aligned `u32` storage, including line padding before the final line.
    pub fn u32(&self) -> Result<(&[u32], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows one-byte storage.
    pub fn u8(&self) -> (&[u8], usize) {
        (self.bytes, self.stride)
    }

    /// Borrows aligned `f32` storage, including line padding before the final line.
    pub fn f32(&self) -> Result<(&[f32], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows aligned `f64` storage, including line padding before the final line.
    pub fn f64(&self) -> Result<(&[f64], usize), i32> {
        self.cast_with_stride()
    }

    fn cast_with_stride<T>(&self) -> Result<(&[T], usize), i32> {
        if !(self.bytes.as_ptr() as usize).is_multiple_of(align_of::<T>())
            || !self.bytes.len().is_multiple_of(size_of::<T>())
            || !self.stride.is_multiple_of(size_of::<T>())
        {
            return Err(-libc::EINVAL);
        }
        let values = unsafe {
            std::slice::from_raw_parts(
                self.bytes.as_ptr().cast::<T>(),
                self.bytes.len() / size_of::<T>(),
            )
        };
        Ok((values, self.stride / size_of::<T>()))
    }
}

/// Writable validated complete-sample storage.
pub struct OutputFrame<'a> {
    bytes: &'a mut [u8],
    stride: usize,
    lines: usize,
    chunk: &'a mut sys::spa_chunk,
    header: Option<&'a mut sys::spa_meta_header>,
    properties: Option<&'a mut sys::spa_meta>,
    properties_set: bool,
}

impl<'a> OutputFrame<'a> {
    /// Validates one registered SPA output buffer against its negotiated format.
    ///
    /// # Safety
    ///
    /// `buffer` must remain registered and exclusively owned by the node for
    /// the returned mutable borrow.
    pub unsafe fn new(buffer: *mut sys::spa_buffer, format: &Format) -> Result<Self, i32> {
        let buffer = unsafe { buffer.as_mut() }.ok_or(-libc::EINVAL)?;
        if buffer.n_datas != 1 || buffer.datas.is_null() {
            return Err(-libc::EINVAL);
        }
        let data = unsafe { &mut *buffer.datas };
        let chunk = unsafe { data.chunk.as_mut() }.ok_or(-libc::EINVAL)?;
        if !is_cpu_mapped(data.type_) || data.data.is_null() {
            return Err(-libc::EINVAL);
        }
        let line_bytes = format.packed_stride()?;
        let lines = format.line_count()?;
        let stride = write_stride(chunk.stride, line_bytes);
        let span = frame_span(line_bytes, stride, lines)?;
        if span > data.maxsize as usize || span > u32::MAX as usize {
            return Err(-libc::ENOSPC);
        }
        let bytes = unsafe { std::slice::from_raw_parts_mut(data.data.cast::<u8>(), span) };
        let header = unsafe { header_mut(buffer) };
        let properties =
            unsafe { sys::spa_buffer_find_meta(buffer, sys::SPA_META_Properties).as_mut() };
        if let Some(meta) = properties.as_ref() {
            if meta.data.is_null()
                || !(16..=4096).contains(&meta.size)
                || meta.size % 8 != 0
                || (meta.data as usize) % 8 != 0
            {
                return Err(-libc::EINVAL);
            }
        }
        Ok(Self {
            bytes,
            stride,
            lines,
            chunk,
            header,
            properties,
            properties_set: false,
        })
    }

    /// Returns the complete mutable byte span, including inter-line padding.
    pub fn bytes_mut(&mut self) -> &mut [u8] {
        self.bytes
    }

    /// Returns the physical byte distance between storage lines.
    pub const fn stride(&self) -> usize {
        self.stride
    }

    /// Returns the physical storage-line count.
    pub const fn lines(&self) -> usize {
        self.lines
    }

    /// Copies input header metadata when this output has standard Header meta.
    pub fn set_header(&mut self, header: Option<Header>) {
        if let Some(destination) = self.header.as_deref_mut() {
            *destination = header
                .map(Into::into)
                .unwrap_or_else(|| unsafe { std::mem::zeroed() });
        }
    }

    /// Preserve every validated property, including unknown optional fields.
    pub fn set_properties(&mut self, source: Option<&[u8]>) -> Result<(), i32> {
        unsafe {
            copy_properties(source, self.properties.as_deref_mut())?;
        }
        self.properties_set = true;
        Ok(())
    }

    /// Borrows aligned mutable `u8` storage.
    pub fn u8_mut(&mut self) -> (&mut [u8], usize) {
        (self.bytes, self.stride)
    }

    /// Borrows aligned mutable `u16` storage.
    pub fn u16_mut(&mut self) -> Result<(&mut [u16], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows aligned mutable `f32` storage.
    pub fn f32_mut(&mut self) -> Result<(&mut [f32], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows aligned mutable `f64` storage.
    pub fn f64_mut(&mut self) -> Result<(&mut [f64], usize), i32> {
        self.cast_with_stride()
    }

    /// Marks the complete output span valid for publication.
    pub fn commit(&mut self) {
        if !self.properties_set {
            if let Some(meta) = self.properties.as_deref_mut() {
                // Registered Properties allocations have negotiated alignment and bounds.
                unsafe {
                    sys::spa_meta_properties_init(meta);
                }
            }
        }
        self.chunk.offset = 0;
        self.chunk.size = self.bytes.len() as u32;
        self.chunk.stride = self.stride as i32;
        self.chunk.flags = 0;
    }

    fn cast_with_stride<T>(&mut self) -> Result<(&mut [T], usize), i32> {
        if !(self.bytes.as_ptr() as usize).is_multiple_of(align_of::<T>())
            || !self.bytes.len().is_multiple_of(size_of::<T>())
            || !self.stride.is_multiple_of(size_of::<T>())
        {
            return Err(-libc::EINVAL);
        }
        let length = self.bytes.len() / size_of::<T>();
        let values =
            unsafe { std::slice::from_raw_parts_mut(self.bytes.as_mut_ptr().cast(), length) };
        Ok((values, self.stride / size_of::<T>()))
    }
}

unsafe fn properties<'a>(buffer: *const sys::spa_buffer) -> Result<Option<&'a [u8]>, i32> {
    let buffer = unsafe { buffer.as_ref() }.ok_or(-libc::EINVAL)?;
    if buffer.n_metas != 0 && buffer.metas.is_null() {
        return Err(-libc::EINVAL);
    }
    let metas = if buffer.n_metas == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(buffer.metas, buffer.n_metas as usize) }
    };
    let mut allocation = None;
    for meta in metas {
        if matches!(meta.type_, 11 | 12) || matches!(meta.type_ >> 16, 11 | 12) {
            return Err(-libc::EINVAL);
        }
        if meta.type_ != sys::SPA_META_Properties {
            continue;
        }
        if allocation.is_some()
            || meta.data.is_null()
            || !(16..=4096).contains(&meta.size)
            || meta.size % 8 != 0
            || (meta.data as usize) % 8 != 0
        {
            return Err(-libc::EINVAL);
        }
        let storage = unsafe { std::slice::from_raw_parts(meta.data.cast(), meta.size as usize) };
        libspa::buffer::meta::BufferProperties::from_bytes(storage, &[])
            .map_err(|_| -libc::EINVAL)?;
        // Preserve the complete Object; negotiated allocations may have different capacities.
        let pod = unsafe { &*meta.data.cast::<sys::spa_pod>() };
        allocation = Some(&storage[..size_of::<sys::spa_pod>() + pod.size as usize]);
    }
    Ok(allocation)
}

unsafe fn copy_properties(
    source: Option<&[u8]>,
    destination: Option<&mut sys::spa_meta>,
) -> Result<(), i32> {
    let Some(destination) = destination else {
        return if source.is_some() {
            Err(-libc::ENOSPC)
        } else {
            Ok(())
        };
    };
    if destination.data.is_null()
        || !(16..=4096).contains(&destination.size)
        || destination.size % 8 != 0
        || (destination.data as usize) % 8 != 0
    {
        return Err(-libc::EINVAL);
    }
    if let Some(source) = source {
        libspa::buffer::meta::BufferProperties::from_bytes(source, &[])
            .map_err(|_| -libc::EINVAL)?;
        if (destination.size as usize) < source.len() {
            return Err(-libc::ENOSPC);
        }
        if destination.data.cast::<u8>() != source.as_ptr().cast_mut() {
            unsafe {
                std::ptr::copy(source.as_ptr(), destination.data.cast(), source.len());
            }
        }
        unsafe {
            std::ptr::write_bytes(
                destination.data.cast::<u8>().add(source.len()),
                0,
                destination.size as usize - source.len(),
            );
        }
    } else if !unsafe { sys::spa_meta_properties_init(destination) } {
        return Err(-libc::EINVAL);
    }

    Ok(())
}

unsafe fn header(buffer: *const sys::spa_buffer) -> Option<&'static sys::spa_meta_header> {
    unsafe {
        sys::spa_buffer_find_meta_data(
            buffer,
            sys::SPA_META_Header,
            size_of::<sys::spa_meta_header>(),
        )
        .cast::<sys::spa_meta_header>()
        .as_ref()
    }
}

unsafe fn header_mut(buffer: *mut sys::spa_buffer) -> Option<&'static mut sys::spa_meta_header> {
    unsafe {
        sys::spa_buffer_find_meta_data(
            buffer,
            sys::SPA_META_Header,
            size_of::<sys::spa_meta_header>(),
        )
        .cast::<sys::spa_meta_header>()
        .as_mut()
    }
}

fn read_stride(stride: i32, packed: usize) -> Result<usize, i32> {
    if stride == 0 {
        Ok(packed)
    } else {
        let stride = usize::try_from(stride).map_err(|_| -libc::EINVAL)?;
        (stride >= packed).then_some(stride).ok_or(-libc::EINVAL)
    }
}

fn write_stride(stride: i32, packed: usize) -> usize {
    if stride > 0 && stride as usize >= packed {
        stride as usize
    } else {
        packed
    }
}

fn frame_span(line_bytes: usize, stride: usize, lines: usize) -> Result<usize, i32> {
    if lines == 0 {
        return Err(-libc::EINVAL);
    }
    stride
        .checked_mul(lines - 1)
        .and_then(|prefix| prefix.checked_add(line_bytes))
        .ok_or(-libc::EOVERFLOW)
}

/// Returns whether two validated frames overlap in memory.
pub fn overlaps(input: &InputFrame<'_>, output: &OutputFrame<'_>) -> bool {
    let input_start = input.bytes.as_ptr() as usize;
    let input_end = input_start.saturating_add(input.bytes.len());
    let output_start = output.bytes.as_ptr() as usize;
    let output_end = output_start.saturating_add(output.bytes.len());
    input_start < output_end && output_start < input_end
}

/// Returns whether two registered buffers refer to the same first data block.
///
/// # Safety
///
/// Both buffer pointers must remain registered for the duration of this call.
pub unsafe fn shares_data(input: *const sys::spa_buffer, output: *const sys::spa_buffer) -> bool {
    let Some(input) = (unsafe { input.as_ref() }) else {
        return false;
    };
    let Some(output) = (unsafe { output.as_ref() }) else {
        return false;
    };
    if input.n_datas != 1 || input.datas.is_null() || output.n_datas != 1 || output.datas.is_null()
    {
        return false;
    }
    let input_data = unsafe { &*input.datas };
    let output_data = unsafe { &*output.datas };
    !input_data.data.is_null() && input_data.data == output_data.data
}

/// Forwards a complete frame when the input and output buffers share data storage.
///
/// The payload is not copied. Chunk state and standard Header metadata are made
/// visible through the output buffer.
///
/// # Safety
///
/// Both buffers must remain registered and host-owned for the duration of this
/// call. Their shared storage must have been established by SPA buffer
/// allocation negotiation.
pub unsafe fn forward_frame(
    input: *const sys::spa_buffer,
    output: *mut sys::spa_buffer,
    format: &Format,
    frame_header: Option<Header>,
) -> Result<bool, i32> {
    let source = unsafe { InputFrame::new(input, format)? };

    let input = unsafe { input.as_ref() }.ok_or(-libc::EINVAL)?;
    let output = unsafe { output.as_mut() }.ok_or(-libc::EINVAL)?;
    if input.n_datas != 1 || input.datas.is_null() || output.n_datas != 1 || output.datas.is_null()
    {
        return Err(-libc::EINVAL);
    }
    let input_data = unsafe { &*input.datas };
    let output_data = unsafe { &mut *output.datas };
    if !is_cpu_mapped(input_data.type_)
        || !is_cpu_mapped(output_data.type_)
        || input_data.data.is_null()
        || output_data.data.is_null()
        || input_data.data != output_data.data
    {
        return Ok(false);
    }

    let input_chunk = unsafe { input_data.chunk.as_ref() }.ok_or(-libc::EINVAL)?;
    let chunk = *input_chunk;
    let output_chunk = unsafe { output_data.chunk.as_mut() }.ok_or(-libc::EINVAL)?;
    if !std::ptr::eq(input_chunk, output_chunk) {
        *output_chunk = chunk;
    }
    if let (Some(destination), Some(source)) = (unsafe { header_mut(output) }, frame_header) {
        *destination = source.into();
    }
    unsafe {
        copy_properties(
            source.properties(),
            sys::spa_buffer_find_meta(output, sys::SPA_META_Properties).as_mut(),
        )?;
    }
    Ok(true)
}

#[cfg(test)]
mod metadata_tests {
    use super::*;
    use libspa::buffer::meta::{
        AcquisitionDomain, AcquisitionIdentity, BufferProperties, BufferPropertiesBuilder,
    };

    fn bytes(words: &mut [u64]) -> &mut [u8] {
        unsafe {
            std::slice::from_raw_parts_mut(words.as_mut_ptr().cast(), std::mem::size_of_val(words))
        }
    }

    #[test]
    fn properties_copy_preserves_unknown_facts_and_reuse_clears_them() {
        let mut input = [0u64; 64];
        let mut output = [u64::MAX; 64];
        let mut builder = BufferPropertiesBuilder::new(bytes(&mut input)).unwrap();
        builder
            .add_acquisition_identity(AcquisitionIdentity::new(
                AcquisitionDomain::new([1; 16]).unwrap(),
                u64::MAX,
                17,
            ))
            .unwrap();
        builder
            .add(123, 0, sys::SPA_TYPE_Int, &42i32.to_ne_bytes())
            .unwrap();
        builder.finish(&[]).unwrap();
        let mut destination = sys::spa_meta {
            type_: sys::SPA_META_Properties,
            size: 512,
            data: output.as_mut_ptr().cast(),
        };
        unsafe {
            copy_properties(Some(bytes(&mut input)), Some(&mut destination)).unwrap();
        }
        let properties = BufferProperties::from_bytes(bytes(&mut output), &[]).unwrap();
        assert_eq!(
            properties.acquisition_identity().unwrap().generation(),
            u64::MAX
        );
        assert_eq!(properties.get(123).unwrap().get_int().unwrap(), 42);
        unsafe {
            copy_properties(None, Some(&mut destination)).unwrap();
        }
        let properties = BufferProperties::from_bytes(bytes(&mut output), &[]).unwrap();
        assert!(properties.acquisition_identity().is_none());
        assert!(properties.get(123).is_none());
    }

    #[test]
    fn shared_properties_are_preserved_and_missing_destination_fails_closed() {
        let mut input = [0u64; 64];
        let mut builder = BufferPropertiesBuilder::new(bytes(&mut input)).unwrap();
        builder.add_exposure_duration(u64::MAX).unwrap();
        builder.finish(&[]).unwrap();
        let mut destination = sys::spa_meta {
            type_: sys::SPA_META_Properties,
            size: 512,
            data: input.as_mut_ptr().cast(),
        };
        unsafe {
            copy_properties(Some(bytes(&mut input)), Some(&mut destination)).unwrap();
        }
        assert_eq!(
            BufferProperties::from_bytes(bytes(&mut input), &[])
                .unwrap()
                .exposure_duration(),
            Some(u64::MAX)
        );
        assert_eq!(
            unsafe { copy_properties(Some(bytes(&mut input)), None) },
            Err(-libc::ENOSPC)
        );
    }
}

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
    metadata: libspa::buffer::meta::MetadataRecords<'a>,
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
        let metadata = unsafe { metadata(buffer)? };
        let header = unsafe { header(buffer).copied().map(Header::from) };
        Ok(Self {
            bytes,
            stride,
            lines,
            header,
            metadata,
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

    /// Independent records borrowed for the source lease.
    pub fn metadata(&self) -> libspa::buffer::meta::MetadataRecords<'_> {
        self.metadata
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
    metadata: &'a mut [sys::spa_meta],
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
        if buffer.n_metas > sys::SPA_META_MAX_RECORDS
            || (buffer.n_metas != 0 && buffer.metas.is_null())
            || !unsafe { sys::spa_meta_records_layout_validate(buffer.metas, buffer.n_metas) }
        {
            return Err(-libc::EINVAL);
        }
        let metadata = if buffer.n_metas == 0 {
            &mut []
        } else {
            unsafe { std::slice::from_raw_parts_mut(buffer.metas, buffer.n_metas as usize) }
        };
        if !metadata
            .iter()
            .any(|record| record.type_ == sys::SPA_META_Header)
        {
            return Err(-libc::EINVAL);
        }
        for record in metadata
            .iter_mut()
            .filter(|record| transferable(record.type_))
        {
            if !unsafe { sys::spa_meta_record_init(record) } {
                return Err(-libc::EINVAL);
            }
        }
        Ok(Self {
            bytes,
            stride,
            lines,
            chunk,
            metadata,
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

    /// Replaces the output publication Header in its sole negotiated record.
    pub fn set_header(&mut self, header: Option<Header>) {
        if let Some(meta) = self
            .metadata
            .iter_mut()
            .find(|meta| meta.type_ == sys::SPA_META_Header)
        {
            unsafe {
                *meta.data.cast::<sys::spa_meta_header>() =
                    header.map(Into::into).unwrap_or_else(|| std::mem::zeroed());
            }
        }
    }

    /// Preserve every accepted record, including opaque optional payloads.
    pub fn set_metadata(
        &mut self,
        source: libspa::buffer::meta::MetadataRecords<'_>,
    ) -> Result<(), i32> {
        unsafe {
            copy_metadata(source.as_raw(), self.metadata)?;
        }
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

unsafe fn metadata<'a>(
    buffer: &sys::spa_buffer,
) -> Result<libspa::buffer::meta::MetadataRecords<'a>, i32> {
    if buffer.n_metas > sys::SPA_META_MAX_RECORDS || (buffer.n_metas != 0 && buffer.metas.is_null())
    {
        return Err(-libc::EINVAL);
    }
    let metas = if buffer.n_metas == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(buffer.metas, buffer.n_metas as usize) }
    };
    let required = [sys::spa_meta_requirement {
        type_: sys::SPA_META_Header,
        size: 32,
        flags: sys::SPA_META_REQUIREMENT_REQUIRED,
        reserved: 0,
    }];
    unsafe { libspa::buffer::meta::MetadataRecords::from_raw(metas, &required) }
        .map_err(|_| -libc::EINVAL)
}

unsafe fn header<'a>(buffer: *const sys::spa_buffer) -> Option<&'a sys::spa_meta_header> {
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

unsafe fn header_mut<'a>(buffer: *mut sys::spa_buffer) -> Option<&'a mut sys::spa_meta_header> {
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
    let destination = if output.n_metas == 0 {
        &mut []
    } else {
        if output.n_metas > sys::SPA_META_MAX_RECORDS || output.metas.is_null() {
            return Err(-libc::EINVAL);
        }
        unsafe { std::slice::from_raw_parts_mut(output.metas, output.n_metas as usize) }
    };
    unsafe {
        copy_metadata(source.metadata().as_raw(), destination)?;
    }
    if let (Some(destination), Some(header)) = (unsafe { header_mut(output) }, frame_header) {
        *destination = header.into();
    }
    if !std::ptr::eq(input_chunk, output_chunk) {
        *output_chunk = chunk;
    }
    Ok(true)
}

fn transferable(type_id: u32) -> bool {
    !unsafe { sys::spa_meta_type_is_transport_owned(type_id) }
}

unsafe fn copy_metadata(
    source: &[sys::spa_meta],
    destination: &mut [sys::spa_meta],
) -> Result<(), i32> {
    let mut src: [sys::spa_meta; 64] = unsafe { std::mem::zeroed() };
    let mut dst = src;
    let (mut ns, mut nd) = (0, 0);
    for record in source.iter().filter(|r| transferable(r.type_)) {
        src[ns] = *record;
        ns += 1;
    }
    for record in destination.iter().filter(|r| transferable(r.type_)) {
        dst[nd] = *record;
        nd += 1;
    }
    if !unsafe { sys::spa_meta_records_copy(dst.as_mut_ptr(), nd as u32, src.as_ptr(), ns as u32) }
    {
        return Err(-libc::ENOSPC);
    }
    Ok(())
}

/// Fixed-capacity owned metadata retained across row-block buffer leases.
/// Publication Header and endpoint-local synchronization are handled separately.
pub struct RetainedMetadata {
    layouts: [[u32; 3]; 64],
    count: usize,
    storage: [u64; 512],
}
impl Default for RetainedMetadata {
    fn default() -> Self {
        Self {
            layouts: [[0; 3]; 64],
            count: 0,
            storage: [0; 512],
        }
    }
}
impl RetainedMetadata {
    /// Clear retained facts on a reset or incomplete frame.
    pub fn clear(&mut self) {
        self.count = 0;
        self.storage.fill(0);
    }
    /// Copy every accepted non-Header source record within the fixed budget.
    pub fn capture(
        &mut self,
        source: libspa::buffer::meta::MetadataRecords<'_>,
    ) -> Result<(), i32> {
        self.clear();
        let mut offset = 0usize;
        for record in source.iter().filter(|record| {
            transferable(record.type_id()) && record.type_id() != sys::SPA_META_Header
        }) {
            let end = offset
                .checked_add(record.size().next_multiple_of(8))
                .ok_or(-libc::ENOSPC)?;
            if self.count == 64 || end > 4096 {
                self.clear();
                return Err(-libc::ENOSPC);
            }
            self.layouts[self.count] = [record.type_id(), record.size() as u32, offset as u32];
            unsafe {
                std::ptr::copy_nonoverlapping(
                    record.bytes().expect("immutable fact").as_ptr(),
                    self.storage.as_mut_ptr().cast::<u8>().add(offset),
                    record.size(),
                );
            }
            self.count += 1;
            offset = end;
        }
        Ok(())
    }
    /// Compare scientific records while allowing each block's Header to vary.
    pub fn matches(&self, source: libspa::buffer::meta::MetadataRecords<'_>) -> bool {
        let mut count = 0;
        for record in source.iter().filter(|record| {
            transferable(record.type_id()) && record.type_id() != sys::SPA_META_Header
        }) {
            let Some(layout) = self.layouts[..self.count]
                .iter()
                .find(|layout| layout[0] == record.type_id())
            else {
                return false;
            };
            if layout[1] as usize != record.size() {
                return false;
            }
            let bytes = unsafe {
                std::slice::from_raw_parts(
                    self.storage.as_ptr().cast::<u8>().add(layout[2] as usize),
                    layout[1] as usize,
                )
            };
            if Some(bytes) != record.bytes() {
                return false;
            }
            count += 1;
        }
        count == self.count
    }
    /// Populate the owned output's negotiated records; reject missing capacity.
    pub fn copy_to(&self, output: &mut OutputFrame<'_>) -> Result<(), i32> {
        let mut raw: [sys::spa_meta; 64] = unsafe { std::mem::zeroed() };
        for (record, layout) in raw.iter_mut().zip(&self.layouts[..self.count]) {
            *record = sys::spa_meta {
                type_: layout[0],
                size: layout[1],
                data: unsafe {
                    self.storage
                        .as_ptr()
                        .cast::<u8>()
                        .add(layout[2] as usize)
                        .cast_mut()
                        .cast()
                },
            };
        }
        let records =
            unsafe { libspa::buffer::meta::MetadataRecords::from_raw(&raw[..self.count], &[]) }
                .map_err(|_| -libc::EINVAL)?;
        output.set_metadata(records)
    }
}

#[cfg(test)]
mod metadata_tests {
    use super::*;
    #[test]
    fn retained_records_own_opaque_bytes_and_ignore_per_block_header_changes() {
        let mut header: sys::spa_meta_header = unsafe { std::mem::zeroed() };
        let mut payload = [u64::MAX, 0x12345678];
        let raw = [
            sys::spa_meta {
                type_: sys::SPA_META_Header,
                size: 32,
                data: std::ptr::from_mut(&mut header).cast(),
            },
            sys::spa_meta {
                type_: 0x3fe,
                size: 16,
                data: payload.as_mut_ptr().cast(),
            },
        ];
        let mut retained = RetainedMetadata::default();
        let records =
            unsafe { libspa::buffer::meta::MetadataRecords::from_raw(&raw, &[]) }.unwrap();
        retained.capture(records).unwrap();
        assert!(retained.matches(records));
        unsafe {
            (*raw[0].data.cast::<sys::spa_meta_header>()).offset = 42;
        }
        let records =
            unsafe { libspa::buffer::meta::MetadataRecords::from_raw(&raw, &[]) }.unwrap();
        assert!(retained.matches(records));
        unsafe {
            *raw[1].data.cast::<u64>() = 99;
        }
        let records =
            unsafe { libspa::buffer::meta::MetadataRecords::from_raw(&raw, &[]) }.unwrap();
        assert!(!retained.matches(records));
        retained.clear();
        assert!(!retained.matches(records));
        assert!(retained.matches(
            unsafe { libspa::buffer::meta::MetadataRecords::from_raw(&raw[..1], &[]) }.unwrap()
        ));
    }
}

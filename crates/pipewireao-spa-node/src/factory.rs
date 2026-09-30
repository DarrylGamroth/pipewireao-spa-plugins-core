//! Generic Rust implementation of SPA factory and node callbacks.

use std::cell::UnsafeCell;
use std::ffi::{CStr, c_char, c_void};
use std::marker::PhantomData;
use std::mem::{ManuallyDrop, size_of};
use std::ops::{Deref, DerefMut};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};

use libspa::pod::Value;
use libspa::sys;

use crate::Format;
use crate::pod;
use crate::port::{MAX_BUFFERS, Port, PortRef, param_info};

/// Behavior supplied by one concrete SPA node factory.
pub trait Node: Send + Sized + 'static {
    /// Whether the node exposes `PropInfo` and `Props` parameters.
    const HAS_PROPS: bool = false;

    /// Whether the node consumes graph Position I/O.
    const NEEDS_POSITION: bool = false;

    /// Constructs one factory instance.
    fn new(info: Option<&sys::spa_dict>) -> Result<Self, i32>;

    /// Returns fixed ports in stable storage.
    fn ports(&self) -> &[Port];

    /// Returns fixed ports in stable storage mutably.
    fn ports_mut(&mut self) -> &mut [Port];

    /// Enumerates one node-level parameter value.
    fn enum_param(&self, _id: u32, _index: u32) -> Result<Option<Value>, i32> {
        Err(-libc::ENOENT)
    }

    /// Applies or tests one node-level parameter.
    fn set_param(
        &mut self,
        _id: u32,
        _flags: u32,
        _value: Option<Value>,
        _started: bool,
    ) -> Result<(), i32> {
        Err(-libc::ENOENT)
    }

    /// Installs node-level I/O selected by this implementation.
    fn set_io(&mut self, _id: u32, _data: *mut c_void, _size: usize) -> Result<(), i32> {
        Err(-libc::ENOENT)
    }

    /// Validates a proposed fixed format before it is installed.
    fn validate_format(&self, _port: usize, _format: Option<&Format>) -> Result<(), i32> {
        Ok(())
    }

    /// Updates prepared storage after one port format changes.
    fn format_changed(&mut self, _port: usize) -> Result<(), i32> {
        Ok(())
    }

    /// Validates node-specific readiness before `Start` succeeds.
    fn ready(&self) -> Result<(), i32> {
        Ok(())
    }

    /// Performs node-specific start preparation.
    fn start(&mut self) -> Result<(), i32> {
        Ok(())
    }

    /// Performs node-specific pause handling.
    fn pause(&mut self) {}

    /// Runs one complete-frame processing step.
    fn process(&mut self) -> Result<i32, i32>;
}

struct State<N> {
    node: N,
    started: bool,
    params: [sys::spa_param_info; 3],
    n_params: u32,
}

/// Serializes callbacks without making a real-time caller wait.
struct CallbackGate<T> {
    claimed: AtomicBool,
    value: UnsafeCell<T>,
}

impl<T> CallbackGate<T> {
    fn new(value: T) -> Self {
        Self {
            claimed: AtomicBool::new(false),
            value: UnsafeCell::new(value),
        }
    }

    fn claim(&self) -> Result<CallbackGuard<'_, T>, i32> {
        self.claimed
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .map_err(|_| -libc::EBUSY)?;
        Ok(CallbackGuard { gate: self })
    }
}

// The atomic claim gives one callback exclusive access to the contained value.
unsafe impl<T: Send> Sync for CallbackGate<T> {}

struct CallbackGuard<'a, T> {
    gate: &'a CallbackGate<T>,
}

impl<T> Deref for CallbackGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        unsafe { &*self.gate.value.get() }
    }
}

impl<T> DerefMut for CallbackGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe { &mut *self.gate.value.get() }
    }
}

impl<T> Drop for CallbackGuard<'_, T> {
    fn drop(&mut self) {
        self.gate.claimed.store(false, Ordering::Release);
    }
}

impl<N: Node> State<N> {
    fn new(node: N) -> Self {
        let mut params = [param_info(0, 0); 3];
        let mut n_params = 0;
        if N::HAS_PROPS {
            params[0] = param_info(sys::SPA_PARAM_PropInfo, sys::SPA_PARAM_INFO_READ);
            params[1] = param_info(sys::SPA_PARAM_Props, sys::SPA_PARAM_INFO_READWRITE);
            n_params = 2;
        }
        params[n_params] = param_info(sys::SPA_PARAM_IO, sys::SPA_PARAM_INFO_READ);
        Self {
            node,
            started: false,
            params,
            n_params: (n_params + 1) as u32,
        }
    }

    fn node_info(&self) -> NodeInfoSnapshot {
        let input_count = self
            .node
            .ports()
            .iter()
            .filter(|port| port.key.direction == sys::SPA_DIRECTION_INPUT)
            .count();
        let output_count = self.node.ports().len() - input_count;
        let info = sys::spa_node_info {
            max_input_ports: input_count as u32,
            max_output_ports: output_count as u32,
            change_mask: 0,
            flags: self.node_flags(),
            props: ptr::null_mut(),
            params: ptr::null_mut(),
            n_params: self.n_params,
        };
        NodeInfoSnapshot {
            info,
            params: self.params,
        }
    }

    fn node_flags(&self) -> u64 {
        let mut flags = sys::SPA_NODE_FLAG_RT as u64;
        if self
            .node
            .ports()
            .iter()
            .any(|port| (port.required || port.format.is_some()) && !port.ready())
        {
            flags |= sys::SPA_NODE_FLAG_NEED_CONFIGURE as u64;
        }
        flags
    }

    fn ready(&self) -> Result<(), i32> {
        if self
            .node
            .ports()
            .iter()
            .any(|port| (port.required || port.format.is_some()) && !port.ready())
        {
            return Err(-libc::EIO);
        }
        self.node.ready()
    }
}

// Event callbacks receive callback-scoped records. These copies keep parameter
// pointers independent of State after releasing its callback gate.
struct NodeInfoSnapshot {
    info: sys::spa_node_info,
    params: [sys::spa_param_info; 3],
}

impl NodeInfoSnapshot {
    fn info(&mut self) -> *const sys::spa_node_info {
        self.info.params = self.params.as_mut_ptr();
        ptr::from_ref(&self.info)
    }
}

struct PortInfoSnapshot {
    key: PortRef,
    info: sys::spa_port_info,
    params: [sys::spa_param_info; 5],
}

impl PortInfoSnapshot {
    fn new(port: &Port) -> Self {
        let mut info = port.info;
        info.params = ptr::null_mut();
        Self {
            key: port.key,
            info,
            params: port.params,
        }
    }

    fn info(&mut self) -> *const sys::spa_port_info {
        self.info.params = self.params.as_mut_ptr();
        self.info.n_params = self.params.len() as u32;
        ptr::from_ref(&self.info)
    }
}

#[repr(C)]
struct Handle<N: Node> {
    handle: sys::spa_handle,
    node: sys::spa_node,
    hooks: UnsafeCell<sys::spa_hook_list>,
    state: ManuallyDrop<CallbackGate<State<N>>>,
}

#[repr(transparent)]
struct SyncValue<T>(T);

// Factory and method tables are immutable for the process lifetime.
unsafe impl<T> Sync for SyncValue<T> {}

struct Methods<N>(PhantomData<N>);

impl<N: Node> Methods<N> {
    const VALUE: sys::spa_node_methods = sys::spa_node_methods {
        version: sys::SPA_VERSION_NODE_METHODS,
        add_listener: Some(node_add_listener::<N>),
        set_callbacks: Some(node_set_callbacks::<N>),
        sync: None,
        enum_params: Some(node_enum_params::<N>),
        set_param: Some(node_set_param::<N>),
        set_io: Some(node_set_io::<N>),
        send_command: Some(node_send_command::<N>),
        add_port: Some(node_add_port::<N>),
        remove_port: Some(node_remove_port::<N>),
        port_enum_params: Some(node_port_enum_params::<N>),
        port_set_param: Some(node_port_set_param::<N>),
        port_use_buffers: Some(node_port_use_buffers::<N>),
        port_set_io: Some(node_port_set_io::<N>),
        port_reuse_buffer: Some(node_port_reuse_buffer::<N>),
        process: Some(node_process::<N>),
    };
}

static INTERFACE_INFO: SyncValue<sys::spa_interface_info> = SyncValue(sys::spa_interface_info {
    type_: sys::SPA_TYPE_INTERFACE_Node.as_ptr().cast(),
});

/// Immutable SPA factory table for one concrete Rust [`Node`].
#[repr(transparent)]
pub struct Factory(SyncValue<sys::spa_handle_factory>);

impl Factory {
    /// Creates a process-static factory table.
    ///
    /// `name` must include one trailing nul byte and no interior nul byte.
    pub const fn new<N: Node>(name: &'static [u8]) -> Self {
        Self(SyncValue(sys::spa_handle_factory {
            version: sys::SPA_VERSION_HANDLE_FACTORY,
            name: name.as_ptr().cast(),
            info: ptr::null(),
            get_size: Some(factory_get_size::<N>),
            init: Some(factory_init::<N>),
            enum_interface_info: Some(factory_enum_interface_info),
        }))
    }

    /// Returns the raw immutable factory pointer expected by SPA loaders.
    pub const fn as_ptr(&'static self) -> *const sys::spa_handle_factory {
        &self.0.0
    }
}

fn ffi_result(operation: impl FnOnce() -> Result<i32, i32>) -> i32 {
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(Ok(result)) => result,
        Ok(Err(error)) => error,
        Err(_) => -libc::EIO,
    }
}

unsafe fn instance_ref<'a, N: Node>(object: *mut c_void) -> Result<&'a Handle<N>, i32> {
    unsafe { object.cast::<Handle<N>>().as_ref() }.ok_or(-libc::EINVAL)
}

fn hooks<N: Node>(instance: &Handle<N>) -> *mut sys::spa_hook_list {
    instance.hooks.get()
}

fn claim<N: Node>(instance: &Handle<N>) -> Result<CallbackGuard<'_, State<N>>, i32> {
    instance.state.claim()
}

unsafe fn for_each_node_event(
    hooks: *mut sys::spa_hook_list,
    mut operation: impl FnMut(&sys::spa_node_events, *mut c_void),
) {
    unsafe {
        let sentinel = ptr::addr_of_mut!((*hooks).list);
        let mut link = (*sentinel).next;
        while link != sentinel {
            let next = (*link).next;
            let hook = link.cast::<sys::spa_hook>();
            let events = (*hook).cb.funcs.cast::<sys::spa_node_events>();
            if let Some(events) = events.as_ref() {
                operation(events, (*hook).cb.data);
            }
            link = next;
        }
    }
}

unsafe fn emit_node_info(hooks: *mut sys::spa_hook_list, info: *const sys::spa_node_info) {
    unsafe {
        for_each_node_event(hooks, |events, data| {
            if let Some(callback) = events.info {
                callback(data, info);
            }
        });
    }
}

unsafe fn emit_port_info(
    hooks: *mut sys::spa_hook_list,
    key: PortRef,
    info: *const sys::spa_port_info,
) {
    unsafe {
        for_each_node_event(hooks, |events, data| {
            if let Some(callback) = events.port_info {
                callback(data, key.direction, key.id, info);
            }
        });
    }
}

unsafe fn emit_result_param(
    hooks: *mut sys::spa_hook_list,
    seq: i32,
    id: u32,
    index: u32,
    param: *mut sys::spa_pod,
) {
    let result = sys::spa_result_node_params {
        id,
        index,
        next: index + 1,
        param,
    };
    unsafe {
        for_each_node_event(hooks, |events, data| {
            if let Some(callback) = events.result {
                callback(
                    data,
                    seq,
                    0,
                    sys::SPA_RESULT_TYPE_NODE_PARAMS,
                    ptr::from_ref(&result).cast(),
                );
            }
        });
    }
}

unsafe fn emit_param(
    hooks: *mut sys::spa_hook_list,
    seq: i32,
    id: u32,
    index: u32,
    value: &Value,
    filter: *const sys::spa_pod,
) -> Result<bool, i32> {
    unsafe {
        pod::with_filtered_pod(value, filter, |param| {
            emit_result_param(hooks, seq, id, index, param);
        })
        .map(|result| result.is_some())
    }
}

unsafe extern "C" fn factory_get_size<N: Node>(
    _factory: *const sys::spa_handle_factory,
    _params: *const sys::spa_dict,
) -> usize {
    size_of::<Handle<N>>()
}

unsafe extern "C" fn factory_init<N: Node>(
    _factory: *const sys::spa_handle_factory,
    handle: *mut sys::spa_handle,
    info: *const sys::spa_dict,
    _support: *const sys::spa_support,
    _n_support: u32,
) -> i32 {
    ffi_result(|| unsafe {
        if handle.is_null() {
            return Err(-libc::EINVAL);
        }
        let instance = handle.cast::<Handle<N>>();
        ptr::write(
            instance,
            Handle {
                handle: sys::spa_handle {
                    version: sys::SPA_VERSION_HANDLE,
                    get_interface: Some(handle_get_interface::<N>),
                    clear: Some(handle_clear::<N>),
                },
                node: sys::spa_node {
                    iface: sys::spa_interface {
                        type_: sys::SPA_TYPE_INTERFACE_Node.as_ptr().cast(),
                        version: sys::SPA_VERSION_NODE,
                        cb: sys::spa_callbacks {
                            funcs: ptr::from_ref(&Methods::<N>::VALUE).cast(),
                            data: instance.cast(),
                        },
                    },
                },
                hooks: UnsafeCell::new(std::mem::zeroed()),
                state: ManuallyDrop::new(CallbackGate::new(State::new(N::new(info.as_ref())?))),
            },
        );
        sys::spa_hook_list_init((*instance).hooks.get());
        Ok(0)
    })
}

unsafe extern "C" fn factory_enum_interface_info(
    _factory: *const sys::spa_handle_factory,
    info: *mut *const sys::spa_interface_info,
    index: *mut u32,
) -> i32 {
    ffi_result(|| unsafe {
        if info.is_null() || index.is_null() {
            return Err(-libc::EINVAL);
        }
        if *index != 0 {
            return Ok(0);
        }
        *info = &INTERFACE_INFO.0;
        *index = 1;
        Ok(1)
    })
}

unsafe extern "C" fn handle_get_interface<N: Node>(
    handle: *mut sys::spa_handle,
    type_: *const c_char,
    interface: *mut *mut c_void,
) -> i32 {
    ffi_result(|| unsafe {
        if handle.is_null() || type_.is_null() || interface.is_null() {
            return Err(-libc::EINVAL);
        }
        let requested = CStr::from_ptr(type_);
        let node_type = CStr::from_bytes_with_nul(sys::SPA_TYPE_INTERFACE_Node)
            .expect("SPA node type is nul-terminated");
        if requested != node_type {
            return Err(-libc::ENOENT);
        }
        let instance = handle.cast::<Handle<N>>();
        *interface = ptr::addr_of_mut!((*instance).node).cast();
        Ok(0)
    })
}

unsafe extern "C" fn handle_clear<N: Node>(handle: *mut sys::spa_handle) -> i32 {
    ffi_result(|| unsafe {
        if handle.is_null() {
            return Err(-libc::EINVAL);
        }
        let instance = handle.cast::<Handle<N>>();
        ManuallyDrop::drop(&mut (*instance).state);
        Ok(0)
    })
}

unsafe extern "C" fn node_add_listener<N: Node>(
    object: *mut c_void,
    listener: *mut sys::spa_hook,
    events: *const sys::spa_node_events,
    data: *mut c_void,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_ref::<N>(object)?;
        if listener.is_null() || events.is_null() {
            return Err(-libc::EINVAL);
        }
        let (mut node_info, port_info) = {
            let state = claim(instance)?;
            let node_info = state.node_info();
            let port_info = state
                .node
                .ports()
                .iter()
                .map(PortInfoSnapshot::new)
                .collect::<Vec<_>>();
            (node_info, port_info)
        };
        let mut saved = std::mem::zeroed();
        sys::spa_hook_list_isolate(hooks(instance), &mut saved, listener, events.cast(), data);
        node_info.info.change_mask =
            (sys::SPA_NODE_CHANGE_MASK_FLAGS | sys::SPA_NODE_CHANGE_MASK_PARAMS) as u64;
        emit_node_info(hooks(instance), node_info.info());
        for mut info in port_info {
            info.info.change_mask =
                (sys::SPA_PORT_CHANGE_MASK_FLAGS | sys::SPA_PORT_CHANGE_MASK_PARAMS) as u64;
            emit_port_info(hooks(instance), info.key, info.info());
        }
        sys::spa_hook_list_join(hooks(instance), &mut saved);
        Ok(0)
    })
}

unsafe extern "C" fn node_set_callbacks<N: Node>(
    object: *mut c_void,
    _callbacks: *const sys::spa_node_callbacks,
    _data: *mut c_void,
) -> i32 {
    ffi_result(|| unsafe {
        instance_ref::<N>(object)?;
        Ok(0)
    })
}

fn generic_or_node_param<N: Node>(
    instance: &Handle<N>,
    id: u32,
    index: u32,
) -> Result<Option<Value>, i32> {
    match (id, index) {
        (sys::SPA_PARAM_IO, 0) => Ok(Some(pod::position_io())),
        (sys::SPA_PARAM_IO, _) => Ok(None),
        _ => claim(instance)?.node.enum_param(id, index),
    }
}

unsafe extern "C" fn node_enum_params<N: Node>(
    object: *mut c_void,
    seq: i32,
    id: u32,
    start: u32,
    max: u32,
    filter: *const sys::spa_pod,
) -> i32 {
    ffi_result(|| unsafe {
        if max == 0 {
            return Err(-libc::EINVAL);
        }
        let instance = instance_ref::<N>(object)?;
        let mut index = start;
        let mut emitted = 0;
        while emitted < max {
            let value = generic_or_node_param(instance, id, index)?;
            let Some(value) = value else {
                break;
            };
            if emit_param(hooks(instance), seq, id, index, &value, filter)? {
                emitted += 1;
            }
            index += 1;
        }
        Ok(0)
    })
}

unsafe extern "C" fn node_set_param<N: Node>(
    object: *mut c_void,
    id: u32,
    flags: u32,
    param: *const sys::spa_pod,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_ref::<N>(object)?;
        let value = if param.is_null() {
            None
        } else {
            Some(pod::decode(param)?)
        };
        let mut state = claim(instance)?;
        let started = state.started;
        state.node.set_param(id, flags, value, started)?;
        Ok(0)
    })
}

unsafe extern "C" fn node_set_io<N: Node>(
    object: *mut c_void,
    id: u32,
    data: *mut c_void,
    size: usize,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_ref::<N>(object)?;
        if id != sys::SPA_IO_Position {
            return Err(-libc::ENOENT);
        }
        if !data.is_null() && size < size_of::<sys::spa_io_position>() {
            return Err(-libc::ENOSPC);
        }
        // Nonconsumers acknowledge the graph driver without retaining its IO.
        if !N::NEEDS_POSITION {
            return Ok(0);
        }
        let mut state = claim(instance)?;
        state.node.set_io(id, data, size)?;
        Ok(0)
    })
}

unsafe extern "C" fn node_send_command<N: Node>(
    object: *mut c_void,
    command: *const sys::spa_command,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_ref::<N>(object)?;
        if command.is_null() {
            return Err(-libc::EINVAL);
        }
        let mut state = claim(instance)?;
        match sys::spa_node_command_id(command.cast_mut()) {
            sys::SPA_NODE_COMMAND_Start => {
                if state.started {
                    return Ok(0);
                }
                state.ready()?;
                state.node.start()?;
                state.started = true;
                Ok(0)
            }
            sys::SPA_NODE_COMMAND_Pause => {
                if state.started {
                    state.started = false;
                    state.node.pause();
                }
                Ok(0)
            }
            _ => Err(-libc::ENOTSUP),
        }
    })
}

unsafe extern "C" fn node_add_port<N: Node>(
    object: *mut c_void,
    _direction: sys::spa_direction,
    _port_id: u32,
    _props: *const sys::spa_dict,
) -> i32 {
    ffi_result(|| unsafe {
        instance_ref::<N>(object)?;
        Err(-libc::ENOTSUP)
    })
}

unsafe extern "C" fn node_remove_port<N: Node>(
    object: *mut c_void,
    _direction: sys::spa_direction,
    _port_id: u32,
) -> i32 {
    ffi_result(|| unsafe {
        instance_ref::<N>(object)?;
        Err(-libc::ENOTSUP)
    })
}

fn port_index<N: Node>(node: &N, direction: sys::spa_direction, id: u32) -> Result<usize, i32> {
    node.ports()
        .iter()
        .position(|port| port.key == (PortRef { direction, id }))
        .ok_or(-libc::EINVAL)
}

unsafe extern "C" fn node_port_enum_params<N: Node>(
    object: *mut c_void,
    seq: i32,
    direction: sys::spa_direction,
    port_id: u32,
    id: u32,
    start: u32,
    max: u32,
    filter: *const sys::spa_pod,
) -> i32 {
    ffi_result(|| unsafe {
        if max == 0 {
            return Err(-libc::EINVAL);
        }
        let instance = instance_ref::<N>(object)?;
        let mut index = start;
        let mut emitted = 0;
        while emitted < max {
            let value = {
                let state = claim(instance)?;
                let port = &state.node.ports()[port_index(&state.node, direction, port_id)?];
                pod::port_param(
                    id,
                    index,
                    port.key.direction,
                    &port.constraints,
                    port.format.as_ref(),
                )?
            };
            let Some(value) = value else {
                break;
            };
            if emit_param(hooks(instance), seq, id, index, &value, filter)? {
                emitted += 1;
            }
            index += 1;
        }
        Ok(0)
    })
}

unsafe extern "C" fn node_port_set_param<N: Node>(
    object: *mut c_void,
    direction: sys::spa_direction,
    port_id: u32,
    id: u32,
    flags: u32,
    param: *const sys::spa_pod,
) -> i32 {
    ffi_result(|| unsafe {
        if id != sys::SPA_PARAM_Format {
            return Err(-libc::ENOENT);
        }
        let instance = instance_ref::<N>(object)?;
        let mut state = claim(instance)?;
        let index = port_index(&state.node, direction, port_id)?;
        let previous_node_flags = state.node_flags();
        let previous_port_flags = state
            .node
            .ports()
            .iter()
            .map(|port| port.info.flags)
            .collect::<Vec<_>>();
        let format = if param.is_null() {
            None
        } else {
            let value = pod::decode(param)?;
            Some(pod::parse_format(
                value,
                &state.node.ports()[index].constraints,
            )?)
        };
        if state.started {
            let current = state.node.ports()[index].format.as_ref();
            if current != format.as_ref() {
                return Err(-libc::EBUSY);
            }
        }
        state.node.validate_format(index, format.as_ref())?;
        if flags & sys::SPA_NODE_PARAM_FLAG_TEST_ONLY != 0 {
            return Ok(0);
        }
        if state.node.ports()[index].format != format {
            let previous = state.node.ports()[index].format.clone();
            let port = &mut state.node.ports_mut()[index];
            port.clear_buffers();
            port.format = format;
            port.update_format_params();
            if let Err(error) = state.node.format_changed(index) {
                let port = &mut state.node.ports_mut()[index];
                port.format = previous;
                port.update_format_params();
                let _ = state.node.format_changed(index);
                let info = (state.node_flags() != previous_node_flags).then(|| state.node_info());
                drop(state);
                if let Some(mut info) = info {
                    info.info.change_mask = sys::SPA_NODE_CHANGE_MASK_FLAGS as u64;
                    emit_node_info(hooks(instance), info.info());
                }
                return Err(error);
            }
        }
        let port_infos = {
            state
                .node
                .ports_mut()
                .iter_mut()
                .enumerate()
                .filter_map(|(port_index, port)| {
                    let mut change_mask = 0;
                    if port_index == index {
                        change_mask |= sys::SPA_PORT_CHANGE_MASK_PARAMS as u64;
                    }
                    if previous_port_flags[port_index] != port.info.flags {
                        change_mask |= sys::SPA_PORT_CHANGE_MASK_FLAGS as u64;
                    }
                    (change_mask != 0).then(|| {
                        let mut info = PortInfoSnapshot::new(port);
                        info.info.change_mask = change_mask;
                        info
                    })
                })
                .collect::<Vec<_>>()
        };
        let node_info = (state.node_flags() != previous_node_flags).then(|| state.node_info());
        drop(state);
        for mut info in port_infos {
            emit_port_info(hooks(instance), info.key, info.info());
        }
        if let Some(mut info) = node_info {
            info.info.change_mask = sys::SPA_NODE_CHANGE_MASK_FLAGS as u64;
            emit_node_info(hooks(instance), info.info());
        }
        Ok(0)
    })
}

unsafe extern "C" fn node_port_use_buffers<N: Node>(
    object: *mut c_void,
    direction: sys::spa_direction,
    port_id: u32,
    flags: u32,
    buffers: *mut *mut sys::spa_buffer,
    n_buffers: u32,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_ref::<N>(object)?;
        let mut state = claim(instance)?;
        let index = port_index(&state.node, direction, port_id)?;
        let previous_node_flags = state.node_flags();
        if state.started {
            return Err(-libc::EBUSY);
        }
        if n_buffers == 0 {
            state.node.ports_mut()[index].clear_buffers();
            let info = (state.node_flags() != previous_node_flags).then(|| state.node_info());
            drop(state);
            if let Some(mut info) = info {
                info.info.change_mask = sys::SPA_NODE_CHANGE_MASK_FLAGS as u64;
                emit_node_info(hooks(instance), info.info());
            }
            return Ok(0);
        }
        if buffers.is_null() || n_buffers as usize > MAX_BUFFERS {
            return Err(-libc::ENOSPC);
        }
        let minimum_size = state.node.ports()[index]
            .format
            .as_ref()
            .ok_or(-libc::EIO)?
            .packed_bytes()?;
        let supplied = std::slice::from_raw_parts(buffers, n_buffers as usize);
        if flags & sys::SPA_NODE_BUFFERS_FLAG_ALLOC != 0 {
            if !state.node.ports()[index].can_allocate_buffers() {
                return Err(-libc::ENOTSUP);
            }
            let other_direction = if direction == sys::SPA_DIRECTION_INPUT {
                sys::SPA_DIRECTION_OUTPUT
            } else {
                sys::SPA_DIRECTION_INPUT
            };
            let other_index = port_index(&state.node, other_direction, port_id)?;
            let other = &state.node.ports()[other_index];
            if other.n_buffers != supplied.len() {
                return Err(-libc::EIO);
            }
            for (buffer_index, &buffer) in supplied.iter().enumerate() {
                let destination = buffer.as_ref().ok_or(-libc::EINVAL)?;
                let source = other.buffers[buffer_index]
                    .buffer
                    .as_ref()
                    .ok_or(-libc::EINVAL)?;
                if destination.n_datas != 1
                    || destination.datas.is_null()
                    || source.n_datas != 1
                    || source.datas.is_null()
                {
                    return Err(-libc::EINVAL);
                }
                let data = &*source.datas;
                if !crate::buffer::is_cpu_mapped(data.type_)
                    || data.data.is_null()
                    || data.chunk.is_null()
                    || (data.maxsize as usize) < minimum_size
                {
                    return Err(-libc::EINVAL);
                }
            }
            for (buffer_index, &buffer) in supplied.iter().enumerate() {
                let source = other.buffers[buffer_index]
                    .buffer
                    .as_ref()
                    .ok_or(-libc::EINVAL)?;
                let source_data = *source.datas;
                let destination = buffer.as_mut().ok_or(-libc::EINVAL)?;
                *destination.datas = source_data;
            }
        }
        for &buffer in supplied {
            let buffer = buffer.as_ref().ok_or(-libc::EINVAL)?;
            if buffer.n_datas != 1 || buffer.datas.is_null() {
                return Err(-libc::EINVAL);
            }
            let data = &*buffer.datas;
            if !crate::buffer::is_cpu_mapped(data.type_)
                || data.data.is_null()
                || data.chunk.is_null()
                || (data.maxsize as usize) < minimum_size
            {
                return Err(-libc::EINVAL);
            }
        }
        let port = &mut state.node.ports_mut()[index];
        port.clear_buffers();
        for (buffer_index, &buffer) in supplied.iter().enumerate() {
            port.buffers[buffer_index].buffer = buffer;
            port.buffers[buffer_index].available = direction == sys::SPA_DIRECTION_OUTPUT;
        }
        port.n_buffers = supplied.len();
        let info = (state.node_flags() != previous_node_flags).then(|| state.node_info());
        drop(state);
        if let Some(mut info) = info {
            info.info.change_mask = sys::SPA_NODE_CHANGE_MASK_FLAGS as u64;
            emit_node_info(hooks(instance), info.info());
        }
        Ok(0)
    })
}

unsafe extern "C" fn node_port_set_io<N: Node>(
    object: *mut c_void,
    direction: sys::spa_direction,
    port_id: u32,
    id: u32,
    data: *mut c_void,
    size: usize,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_ref::<N>(object)?;
        let mut state = claim(instance)?;
        let index = port_index(&state.node, direction, port_id)?;
        let previous_node_flags = state.node_flags();
        if state.started {
            return Err(-libc::EBUSY);
        }
        let port = &mut state.node.ports_mut()[index];
        port.set_io(id, data, size)?;
        let info = (state.node_flags() != previous_node_flags).then(|| state.node_info());
        drop(state);
        if let Some(mut info) = info {
            info.info.change_mask = sys::SPA_NODE_CHANGE_MASK_FLAGS as u64;
            emit_node_info(hooks(instance), info.info());
        }
        Ok(0)
    })
}

unsafe extern "C" fn node_port_reuse_buffer<N: Node>(
    object: *mut c_void,
    port_id: u32,
    buffer_id: u32,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_ref::<N>(object)?;
        let mut state = claim(instance)?;
        let index = port_index(&state.node, sys::SPA_DIRECTION_OUTPUT, port_id)?;
        let port = &mut state.node.ports_mut()[index];
        if buffer_id as usize >= port.n_buffers {
            return Err(-libc::EINVAL);
        }
        port.buffers[buffer_id as usize].available = true;
        Ok(0)
    })
}

unsafe extern "C" fn node_process<N: Node>(object: *mut c_void) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_ref::<N>(object)?;
        let mut state = claim(instance)?;
        if !state.started {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        }
        state.node.process()
    })
}

#[cfg(test)]
mod tests {
    use std::cell::{Cell, UnsafeCell};
    use std::ffi::c_void;
    use std::mem::ManuallyDrop;
    use std::ptr;

    use libspa::sys;

    use super::{
        CallbackGate, Handle, Methods, Node, State, claim, ffi_result, node_add_listener,
        node_port_set_param, node_set_io,
    };
    use crate::{Format, Port, PortRef};

    #[test]
    fn callback_gate_never_waits_for_an_active_callback() {
        let gate = CallbackGate::new(7);
        let mut guard = gate.claim().expect("first callback claims the node");
        *guard += 1;
        assert!(matches!(gate.claim(), Err(error) if error == -libc::EBUSY));
        drop(guard);
        assert_eq!(*gate.claim().expect("released node can be reclaimed"), 8);
    }

    #[test]
    fn callback_panics_are_contained_as_io_errors() {
        assert_eq!(ffi_result(|| panic!("test callback panic")), -libc::EIO);
    }

    struct TestNode {
        ports: [Port; 2],
        reject_format_withdrawal: bool,
    }

    impl Node for TestNode {
        fn new(_: Option<&sys::spa_dict>) -> Result<Self, i32> {
            Ok(Self {
                ports: [
                    Port::new(
                        PortRef {
                            direction: sys::SPA_DIRECTION_INPUT,
                            id: 0,
                        },
                        true,
                        false,
                        [],
                    ),
                    Port::new(
                        PortRef {
                            direction: sys::SPA_DIRECTION_OUTPUT,
                            id: 0,
                        },
                        true,
                        false,
                        [],
                    ),
                ],
                reject_format_withdrawal: false,
            })
        }
        fn ports(&self) -> &[Port] {
            &self.ports
        }
        fn ports_mut(&mut self) -> &mut [Port] {
            &mut self.ports
        }
        fn format_changed(&mut self, port: usize) -> Result<(), i32> {
            if self.reject_format_withdrawal && self.ports[port].format.is_none() {
                Err(-libc::EINVAL)
            } else {
                Ok(())
            }
        }
        fn process(&mut self) -> Result<i32, i32> {
            Ok(sys::SPA_STATUS_NEED_DATA as i32)
        }
    }

    fn test_handle<N: Node>(node: N) -> Box<Handle<N>> {
        let handle = Box::new(Handle {
            handle: unsafe { std::mem::zeroed() },
            node: sys::spa_node {
                iface: sys::spa_interface {
                    type_: ptr::null(),
                    version: sys::SPA_VERSION_NODE,
                    cb: sys::spa_callbacks {
                        funcs: ptr::from_ref(&Methods::<N>::VALUE).cast(),
                        data: ptr::null_mut(),
                    },
                },
            },
            hooks: UnsafeCell::new(unsafe { std::mem::zeroed() }),
            state: ManuallyDrop::new(CallbackGate::new(State::new(node))),
        });
        unsafe { sys::spa_hook_list_init(handle.hooks.get()) };
        handle
    }

    #[test]
    fn failed_format_change_publishes_buffer_withdrawal() {
        unsafe extern "C" fn on_info(data: *mut c_void, info: *const sys::spa_node_info) {
            let flags = unsafe { &mut *data.cast::<u64>() };
            let info = unsafe { &*info };
            if info.change_mask & sys::SPA_NODE_CHANGE_MASK_FLAGS as u64 != 0 {
                *flags = info.flags;
            }
        }

        let mut handle = test_handle(TestNode::new(None).unwrap());
        let object = ptr::from_mut(handle.as_mut()).cast::<c_void>();
        let mut io: sys::spa_io_buffers = unsafe { std::mem::zeroed() };
        let format = Format::f32_image("org.calculon.test/1", 4, 3, None).unwrap();
        {
            let mut state = claim(handle.as_ref()).unwrap();
            state.node.reject_format_withdrawal = true;
            for port in state.node.ports_mut() {
                port.format = Some(format.clone());
                port.n_buffers = 1;
                port.io = ptr::from_mut(&mut io);
            }
            assert!(state.ready().is_ok());
        }
        let mut flags = 0_u64;
        let mut listener: sys::spa_hook = unsafe { std::mem::zeroed() };
        let mut events: sys::spa_node_events = unsafe { std::mem::zeroed() };
        events.version = sys::SPA_VERSION_NODE_EVENTS;
        events.info = Some(on_info);
        assert_eq!(
            unsafe {
                node_add_listener::<TestNode>(
                    object,
                    &mut listener,
                    &events,
                    ptr::from_mut(&mut flags).cast(),
                )
            },
            0
        );
        assert_eq!(flags & sys::SPA_NODE_FLAG_NEED_CONFIGURE as u64, 0);
        assert_eq!(
            unsafe {
                node_port_set_param::<TestNode>(
                    object,
                    sys::SPA_DIRECTION_INPUT,
                    0,
                    sys::SPA_PARAM_Format,
                    0,
                    ptr::null(),
                )
            },
            -libc::EINVAL
        );
        {
            let state = claim(handle.as_ref()).unwrap();
            assert_eq!(state.node.ports()[0].format.as_ref(), Some(&format));
            assert_eq!(state.node.ports()[0].n_buffers, 0);
            assert_eq!(state.ready(), Err(-libc::EIO));
        }
        assert_ne!(flags & sys::SPA_NODE_FLAG_NEED_CONFIGURE as u64, 0);
        unsafe {
            sys::spa_hook_remove(&mut listener);
            ManuallyDrop::drop(&mut handle.state);
        }
    }

    #[test]
    fn info_parameters_survive_reentrant_format_withdrawal() {
        struct Capture {
            object: *mut c_void,
            reenter_on_node: bool,
            armed: Cell<bool>,
            withdrawing: Cell<bool>,
            nested_events: Cell<u32>,
            result: Cell<i32>,
            node_checked: Cell<bool>,
            port_checked: Cell<bool>,
            node_owned: Cell<bool>,
            port_owned: Cell<bool>,
        }

        unsafe fn values(params: *const sys::spa_param_info, count: u32) -> Vec<(u32, u32)> {
            unsafe { std::slice::from_raw_parts(params, count as usize) }
                .iter()
                .map(|param| (param.id, param.flags))
                .collect()
        }

        fn withdraw(capture: &Capture) {
            capture.armed.set(false);
            capture.withdrawing.set(true);
            capture.result.set(unsafe {
                node_port_set_param::<TestNode>(
                    capture.object,
                    sys::SPA_DIRECTION_INPUT,
                    0,
                    sys::SPA_PARAM_Format,
                    0,
                    ptr::null(),
                )
            });
            capture.withdrawing.set(false);
        }

        unsafe extern "C" fn on_info(data: *mut c_void, info: *const sys::spa_node_info) {
            let capture = unsafe { &*data.cast::<Capture>() };
            if capture.withdrawing.get() {
                capture.nested_events.set(capture.nested_events.get() + 1);
                return;
            }
            if !capture.reenter_on_node || !capture.armed.get() {
                return;
            }
            // Copy values rather than retaining a slice across the nested call:
            // the negative case may expose an array that the nested call mutates.
            let before = unsafe { values((*info).params, (*info).n_params) };
            let params = unsafe { (*info).params };
            let flags = unsafe { (*info).flags };
            let instance = unsafe { super::instance_ref::<TestNode>(capture.object).unwrap() };
            {
                let state = claim(instance).unwrap();
                capture
                    .node_owned
                    .set(params != state.params.as_ptr().cast_mut());
            }
            withdraw(capture);
            let after = unsafe { values((*info).params, (*info).n_params) };
            let state = claim(instance).unwrap();
            capture.node_checked.set(
                before == after
                    && before == vec![(sys::SPA_PARAM_IO, sys::SPA_PARAM_INFO_READ)]
                    && params == unsafe { (*info).params }
                    && flags == unsafe { (*info).flags }
                    && flags & sys::SPA_NODE_FLAG_NEED_CONFIGURE as u64 == 0
                    && state.node_flags() & sys::SPA_NODE_FLAG_NEED_CONFIGURE as u64 != 0,
            );
        }

        unsafe extern "C" fn on_port_info(
            data: *mut c_void,
            direction: u32,
            id: u32,
            info: *const sys::spa_port_info,
        ) {
            let capture = unsafe { &*data.cast::<Capture>() };
            if capture.withdrawing.get() {
                capture.nested_events.set(capture.nested_events.get() + 1);
                return;
            }
            if direction != sys::SPA_DIRECTION_INPUT || id != 0 {
                return;
            }
            let before = unsafe { values((*info).params, (*info).n_params) };
            let params = unsafe { (*info).params };
            let instance = unsafe { super::instance_ref::<TestNode>(capture.object).unwrap() };
            {
                let state = claim(instance).unwrap();
                capture
                    .port_owned
                    .set(params != state.node.ports()[0].params.as_ptr().cast_mut());
            }
            if !capture.reenter_on_node && capture.armed.get() {
                withdraw(capture);
            }
            let after = unsafe { values((*info).params, (*info).n_params) };
            let state = claim(instance).unwrap();
            let live = &state.node.ports()[0];
            capture.port_checked.set(
                before == after
                    && before.len() == 5
                    && before[2] == (sys::SPA_PARAM_Format, sys::SPA_PARAM_INFO_READWRITE)
                    && params == unsafe { (*info).params }
                    && live.params[2].flags == sys::SPA_PARAM_INFO_WRITE
                    && live.format.is_none(),
            );
        }

        // One synchronous main-thread listener; node stopped, no processing or
        // hook/handle destruction. Mutation uses the public format-clear method.
        // Cell avoids an exclusive capture borrow spanning nested notifications.
        for reenter_on_node in [true, false] {
            let mut handle = test_handle(TestNode::new(None).unwrap());
            let object = ptr::from_mut(handle.as_mut()).cast::<c_void>();
            let mut io: sys::spa_io_buffers = unsafe { std::mem::zeroed() };
            let format = Format::f32_image("org.calculon.test/1", 4, 3, None).unwrap();
            {
                let mut state = claim(handle.as_ref()).unwrap();
                for port in state.node.ports_mut() {
                    port.format = Some(format.clone());
                    port.update_format_params();
                    port.n_buffers = 1;
                    port.io = ptr::from_mut(&mut io);
                }
            }
            let capture = Capture {
                object,
                reenter_on_node,
                armed: Cell::new(true),
                withdrawing: Cell::new(false),
                nested_events: Cell::new(0),
                result: Cell::new(-libc::EIO),
                node_checked: Cell::new(false),
                port_checked: Cell::new(false),
                node_owned: Cell::new(false),
                port_owned: Cell::new(false),
            };
            let mut listener: sys::spa_hook = unsafe { std::mem::zeroed() };
            let mut events: sys::spa_node_events = unsafe { std::mem::zeroed() };
            events.version = sys::SPA_VERSION_NODE_EVENTS;
            events.info = Some(on_info);
            events.port_info = Some(on_port_info);
            assert_eq!(
                unsafe {
                    node_add_listener::<TestNode>(
                        object,
                        &mut listener,
                        &events,
                        ptr::from_ref(&capture).cast_mut().cast(),
                    )
                },
                0
            );
            assert_eq!(capture.result.get(), 0);
            assert!(capture.nested_events.get() >= 2);
            assert!(capture.port_checked.get());
            assert!(capture.port_owned.get());
            if reenter_on_node {
                assert!(capture.node_checked.get());
                assert!(capture.node_owned.get());
            }
            unsafe {
                sys::spa_hook_remove(&mut listener);
                ManuallyDrop::drop(&mut handle.state);
            }
        }
    }

    #[test]
    fn unused_position_io_is_acknowledged_and_validated() {
        let mut handle = test_handle(TestNode::new(None).unwrap());
        {
            let state = claim(handle.as_ref()).unwrap();
            assert_eq!(state.n_params, 1);
            assert_eq!(state.params[0].id, sys::SPA_PARAM_IO);
        }
        let object = ptr::from_mut(handle.as_mut()).cast::<c_void>();
        let mut position: sys::spa_io_position = unsafe { std::mem::zeroed() };
        let data = ptr::from_mut(&mut position).cast();
        let size = std::mem::size_of_val(&position);
        assert_eq!(
            unsafe { node_set_io::<TestNode>(object, sys::SPA_IO_Position, data, size) },
            0
        );
        assert_eq!(
            unsafe { node_set_io::<TestNode>(object, sys::SPA_IO_Position, data, size - 1) },
            -libc::ENOSPC
        );
        assert_eq!(
            unsafe { node_set_io::<TestNode>(object, sys::SPA_IO_Position, ptr::null_mut(), 0) },
            0
        );
        assert_eq!(
            unsafe { node_set_io::<TestNode>(object, sys::SPA_IO_Clock, data, size) },
            -libc::ENOENT
        );
        assert!(
            super::generic_or_node_param(handle.as_ref(), sys::SPA_PARAM_IO, 0)
                .unwrap()
                .is_some()
        );
        assert!(
            super::generic_or_node_param(handle.as_ref(), sys::SPA_PARAM_IO, 1)
                .unwrap()
                .is_none()
        );
        unsafe { ManuallyDrop::drop(&mut handle.state) };
    }

    #[test]
    fn position_consumers_receive_setup_clear_and_returned_errors() {
        struct Consumer {
            calls: usize,
            position: usize,
        }
        impl Node for Consumer {
            const HAS_PROPS: bool = true;
            const NEEDS_POSITION: bool = true;
            fn new(_: Option<&sys::spa_dict>) -> Result<Self, i32> {
                Ok(Self {
                    calls: 0,
                    position: 0,
                })
            }
            fn ports(&self) -> &[Port] {
                &[]
            }
            fn ports_mut(&mut self) -> &mut [Port] {
                &mut []
            }
            fn set_io(&mut self, id: u32, data: *mut c_void, _: usize) -> Result<(), i32> {
                assert_eq!(id, sys::SPA_IO_Position);
                self.calls += 1;
                self.position = data as usize;
                if data.is_null() {
                    Err(-libc::EIO)
                } else {
                    Ok(())
                }
            }
            fn process(&mut self) -> Result<i32, i32> {
                Ok(sys::SPA_STATUS_NEED_DATA as i32)
            }
        }
        let mut handle = test_handle(Consumer::new(None).unwrap());
        {
            let state = claim(handle.as_ref()).unwrap();
            assert_eq!(state.n_params, 3);
            assert_eq!(state.params[0].id, sys::SPA_PARAM_PropInfo);
            assert_eq!(state.params[1].id, sys::SPA_PARAM_Props);
            assert_eq!(state.params[2].id, sys::SPA_PARAM_IO);
        }
        let object = ptr::from_mut(handle.as_mut()).cast::<c_void>();
        let mut position: sys::spa_io_position = unsafe { std::mem::zeroed() };
        let data = ptr::from_mut(&mut position).cast();
        let size = std::mem::size_of_val(&position);
        assert_eq!(
            unsafe { node_set_io::<Consumer>(object, sys::SPA_IO_Position, data, size) },
            0
        );
        assert_eq!(claim(handle.as_ref()).unwrap().node.position, data as usize);
        assert_eq!(
            unsafe { node_set_io::<Consumer>(object, sys::SPA_IO_Position, data, size - 1) },
            -libc::ENOSPC
        );
        assert_eq!(claim(handle.as_ref()).unwrap().node.calls, 1);
        assert_eq!(
            unsafe { node_set_io::<Consumer>(object, sys::SPA_IO_Position, ptr::null_mut(), 0) },
            -libc::EIO
        );
        assert_eq!(claim(handle.as_ref()).unwrap().node.calls, 2);
        assert_eq!(claim(handle.as_ref()).unwrap().node.position, 0);
        unsafe { ManuallyDrop::drop(&mut handle.state) };
    }
}

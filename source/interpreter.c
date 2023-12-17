#include <uacpi/internal/types.h>
#include <uacpi/internal/interpreter.h>
#include <uacpi/internal/dynamic_array.h>
#include <uacpi/internal/opcodes.h>
#include <uacpi/internal/namespace.h>
#include <uacpi/internal/stdlib.h>
#include <uacpi/internal/context.h>
#include <uacpi/internal/shareable.h>
#include <uacpi/kernel_api.h>

enum item_type {
    ITEM_NONE = 0,
    ITEM_NAMESPACE_NODE,
    ITEM_NAMESPACE_NODE_METHOD_LOCAL,
    ITEM_OBJECT,
    ITEM_EMPTY_OBJECT,
    ITEM_PACKAGE_LENGTH,
    ITEM_IMMEDIATE,
};

struct package_length {
    uacpi_u32 begin;
    uacpi_u32 end;
};

struct item {
    uacpi_u8 type;
    union {
        uacpi_handle handle;
        uacpi_object *obj;
        struct uacpi_namespace_node *node;
        struct package_length pkg;
        uacpi_u64 immediate;
        uacpi_u8 immediate_bytes[8];
    };
};

DYNAMIC_ARRAY_WITH_INLINE_STORAGE(item_array, struct item, 8)
DYNAMIC_ARRAY_WITH_INLINE_STORAGE_IMPL(item_array, struct item, static)

struct op_context {
    uacpi_u8 pc;
    uacpi_bool preempted;

    /*
     * == 0 -> none
     * >= 1 -> item[idx - 1]
     */
    uacpi_u8 tracked_pkg_idx;

    const struct uacpi_op_spec *op;
    struct item_array items;
};

DYNAMIC_ARRAY_WITH_INLINE_STORAGE(op_context_array, struct op_context, 8)
DYNAMIC_ARRAY_WITH_INLINE_STORAGE_IMPL(
    op_context_array, struct op_context, static
)

static struct op_context *op_context_array_one_before_last(
    struct op_context_array *arr
)
{
    uacpi_size size;

    size = op_context_array_size(arr);

    if (size < 2)
        return UACPI_NULL;

    return op_context_array_at(arr, size - 2);
}

enum code_block_type {
    CODE_BLOCK_IF = 1,
    CODE_BLOCK_ELSE = 2,
    CODE_BLOCK_WHILE = 3,
    CODE_BLOCK_SCOPE = 4,
};

struct code_block {
    enum code_block_type type;
    uacpi_u32 begin, end;
    struct uacpi_namespace_node *node;
};

DYNAMIC_ARRAY_WITH_INLINE_STORAGE(code_block_array, struct code_block, 8)
DYNAMIC_ARRAY_WITH_INLINE_STORAGE_IMPL(
    code_block_array, struct code_block, static
)

DYNAMIC_ARRAY_WITH_INLINE_STORAGE(
    temp_namespace_node_array, uacpi_namespace_node*, 8)
DYNAMIC_ARRAY_WITH_INLINE_STORAGE_IMPL(
    temp_namespace_node_array, uacpi_namespace_node*, static
)

static uacpi_status temp_namespace_node_array_push(
    struct temp_namespace_node_array *arr, uacpi_namespace_node *node
)
{
    uacpi_namespace_node **slot;

    slot = temp_namespace_node_array_alloc(arr);
    if (uacpi_unlikely(slot == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    *slot = node;
    return UACPI_STATUS_OK;
}

struct call_frame {
    struct uacpi_control_method *method;

    uacpi_object *args[7];
    uacpi_object *locals[8];

    struct op_context_array pending_ops;
    struct code_block_array code_blocks;
    struct temp_namespace_node_array temp_nodes;
    struct code_block *last_while;
    struct uacpi_namespace_node *cur_scope;

    uacpi_u32 code_offset;
};

static void *call_frame_cursor(struct call_frame *frame)
{
    return frame->method->code + frame->code_offset;
}

static uacpi_size call_frame_code_bytes_left(struct call_frame *frame)
{
    return frame->method->size - frame->code_offset;
}

static bool call_frame_has_code(struct call_frame* frame)
{
    return call_frame_code_bytes_left(frame) > 0;
}

DYNAMIC_ARRAY_WITH_INLINE_STORAGE(call_frame_array, struct call_frame, 4)
DYNAMIC_ARRAY_WITH_INLINE_STORAGE_IMPL(
    call_frame_array, struct call_frame, static
)

// NOTE: Try to keep size under 2 pages
struct execution_context {
    uacpi_object *ret;
    struct call_frame_array call_stack;

    struct call_frame *cur_frame;
    struct code_block *cur_block;
    struct uacpi_control_method *cur_method;
    const struct uacpi_op_spec *cur_op;
    struct op_context *prev_op_ctx;
    struct op_context *cur_op_ctx;

    uacpi_bool skip_else;
};

#define AML_READ(ptr, offset) (*(((uacpi_u8*)(code)) + offset))

/*
 * LeadNameChar := ‘A’-‘Z’ | ‘_’
 * DigitChar := ‘0’ - ‘9’
 * NameChar := DigitChar | LeadNameChar
 * ‘A’-‘Z’ := 0x41 - 0x5A
 * ‘_’ := 0x5F
 * ‘0’-‘9’ := 0x30 - 0x39
 */
static uacpi_status parse_nameseg(uacpi_u8 *cursor,
                                  uacpi_object_name *out_name)
{
    uacpi_size i;

    for (i = 0; i < 4; ++i) {
        uacpi_char data = cursor[i];

        if (data == '_')
            continue;
        if (data >= '0' && data <= '9')
            continue;
        if (data >= 'A' && data <= 'Z')
            continue;

        return UACPI_STATUS_BAD_BYTECODE;
    }

    uacpi_memcpy(&out_name->id, cursor, 4);
    return UACPI_STATUS_OK;
}

/*
 * -------------------------------------------------------------
 * RootChar := ‘\’
 * ParentPrefixChar := ‘^’
 * ‘\’ := 0x5C
 * ‘^’ := 0x5E
 * ------------------------------------------------------------
 * NameSeg := <leadnamechar namechar namechar namechar>
 * NameString := <rootchar namepath> | <prefixpath namepath>
 * PrefixPath := Nothing | <’^’ prefixpath>
 * NamePath := NameSeg | DualNamePath | MultiNamePath | NullName
 * DualNamePath := DualNamePrefix NameSeg NameSeg
 * MultiNamePath := MultiNamePrefix SegCount NameSeg(SegCount)
 */

static uacpi_status name_string_to_path(
    struct call_frame *frame, uacpi_size offset,
    uacpi_char **out_string, uacpi_size *out_size
)
{
    uacpi_status ret = UACPI_STATUS_OK;
    uacpi_size bytes_left, prefix_bytes, nameseg_bytes, namesegs;
    uacpi_char *base_cursor, *cursor;
    uacpi_char prev_char;

    bytes_left = frame->method->size - offset;
    cursor = (uacpi_char*)frame->method->code + offset;
    base_cursor = cursor;
    namesegs = 0;

    prefix_bytes = 0;
    for (;;) {
        if (uacpi_unlikely(bytes_left == 0))
            return UACPI_STATUS_BAD_BYTECODE;

        prev_char = *cursor;

        switch (prev_char) {
        case '^':
        case '\\':
            prefix_bytes++;
            cursor++;
            bytes_left--;
            break;
        default:
            break;
        }

        if (prev_char != '^')
            break;
    }

    // At least a NullName byte is expected here
    if (uacpi_unlikely(bytes_left == 0))
        return UACPI_STATUS_BAD_BYTECODE;

    namesegs = 0;
    bytes_left--;
    switch (*cursor++)
    {
    case UACPI_DUAL_NAME_PREFIX:
        namesegs = 2;
        break;
    case UACPI_MULTI_NAME_PREFIX:
        if (uacpi_unlikely(bytes_left == 0))
            return UACPI_STATUS_BAD_BYTECODE;
        namesegs = *(uacpi_u8*)cursor;
        cursor++;
        bytes_left--;
        break;
    case UACPI_NULL_NAME:
        break;
    default:
        /*
         * Might be an invalid byte, but assume single nameseg for now,
         * the code below will validate it for us.
         */
        cursor--;
        bytes_left++;
        namesegs = 1;
        break;
    }

    if (uacpi_unlikely((namesegs * 4) > bytes_left))
        return UACPI_STATUS_BAD_BYTECODE;

    // 4 chars per nameseg
    nameseg_bytes = namesegs * 4;

    // dot separator for every nameseg
    nameseg_bytes += namesegs - 1;

    *out_size = nameseg_bytes + prefix_bytes + 1;

    *out_string = uacpi_kernel_alloc(*out_size);
    if (*out_string == UACPI_NULL)
        return UACPI_STATUS_OUT_OF_MEMORY;

    uacpi_memcpy(*out_string, base_cursor, prefix_bytes);

    base_cursor = *out_string;
    base_cursor += prefix_bytes;

    while (namesegs-- > 0) {
        uacpi_memcpy(base_cursor, cursor, 4);
        cursor += 4;
        base_cursor += 4;

        if (namesegs)
            *base_cursor++ = '.';
    }

    *base_cursor = '\0';
    return UACPI_STATUS_OK;
}

enum resolve_behavior {
    RESOLVE_CREATE_LAST_NAMESEG_FAIL_IF_EXISTS,
    RESOLVE_FAIL_IF_DOESNT_EXIST,
};

static uacpi_status resolve_name_string(
    struct call_frame *frame,
    enum resolve_behavior behavior,
    struct uacpi_namespace_node **out_node
)
{
    uacpi_status ret = UACPI_STATUS_OK;
    uacpi_u8 *cursor;
    uacpi_size bytes_left, namesegs = 0;
    struct uacpi_namespace_node *parent, *cur_node = frame->cur_scope;
    uacpi_char prev_char = 0;
    uacpi_bool just_one_nameseg = UACPI_TRUE;

    bytes_left = call_frame_code_bytes_left(frame);
    cursor = call_frame_cursor(frame);

    for (;;) {
        if (uacpi_unlikely(bytes_left == 0))
            return UACPI_STATUS_BAD_BYTECODE;

        switch (*cursor) {
        case '\\':
            if (prev_char == '^')
                return UACPI_STATUS_BAD_BYTECODE;

            cur_node = uacpi_namespace_root();
            break;
        case '^':
            // Tried to go behind root
            if (uacpi_unlikely(cur_node == uacpi_namespace_root()))
                return UACPI_STATUS_BAD_BYTECODE;

            cur_node = cur_node->parent;
            break;
        default:
            break;
        }

        prev_char = *cursor;

        switch (prev_char) {
        case '^':
        case '\\':
            just_one_nameseg = UACPI_FALSE;
            cursor++;
            bytes_left--;
            break;
        default:
            break;
        }

        if (prev_char != '^')
            break;
    }

    // At least a NullName byte is expected here
    if (uacpi_unlikely(bytes_left == 0))
        return UACPI_STATUS_BAD_BYTECODE;

    bytes_left--;
    switch (*cursor++)
    {
    case UACPI_DUAL_NAME_PREFIX:
        namesegs = 2;
        just_one_nameseg = UACPI_FALSE;
        break;
    case UACPI_MULTI_NAME_PREFIX:
        if (uacpi_unlikely(bytes_left == 0))
            return UACPI_STATUS_BAD_BYTECODE;
        namesegs = *cursor;
        cursor++;
        bytes_left--;
        just_one_nameseg = UACPI_FALSE;
        break;
    case UACPI_NULL_NAME:
        if (behavior == RESOLVE_CREATE_LAST_NAMESEG_FAIL_IF_EXISTS ||
            just_one_nameseg)
            return UACPI_STATUS_BAD_BYTECODE;

        goto out;
    default:
        /*
         * Might be an invalid byte, but assume single nameseg for now,
         * the code below will validate it for us.
         */
        cursor--;
        bytes_left++;
        namesegs = 1;
        break;
    }

    if (uacpi_unlikely((namesegs * 4) > bytes_left))
        return UACPI_STATUS_BAD_BYTECODE;

    for (; namesegs; cursor += 4, namesegs--) {
        uacpi_object_name name;

        ret = parse_nameseg(cursor, &name);
        if (uacpi_unlikely_error(ret))
            return ret;

        parent = cur_node;
        cur_node = uacpi_namespace_node_find(parent, name);

        switch (behavior) {
        case RESOLVE_CREATE_LAST_NAMESEG_FAIL_IF_EXISTS:
            if (namesegs == 1) {
                if (cur_node)
                    return UACPI_STATUS_ALREADY_EXISTS;

                // Create the node and link to parent but don't install YET
                cur_node = uacpi_namespace_node_alloc(name);
                cur_node->parent = parent;
            }
            break;
        case RESOLVE_FAIL_IF_DOESNT_EXIST:
            if (just_one_nameseg) {
                while (!cur_node && parent != uacpi_namespace_root()) {
                    cur_node = parent;
                    parent = cur_node->parent;

                    cur_node = uacpi_namespace_node_find(parent, name);
                }
            }
            break;
        default:
            return UACPI_STATUS_INVALID_ARGUMENT;
        }

        if (cur_node == UACPI_NULL) {
            ret = UACPI_STATUS_NOT_FOUND;
            break;
        }
    }

out:
    cursor += namesegs * 4;
    frame->code_offset = cursor - frame->method->code;
    *out_node = cur_node;
    return ret;
}

static uacpi_status get_op(struct execution_context *ctx)
{
    uacpi_aml_op op;
    struct call_frame *frame = ctx->cur_frame;
    void *code = frame->method->code;
    uacpi_size size = frame->method->size;

    if (uacpi_unlikely(frame->code_offset >= size))
        return UACPI_STATUS_OUT_OF_BOUNDS;

    op = AML_READ(code, frame->code_offset++);
    if (op == UACPI_EXT_PREFIX) {
        if (uacpi_unlikely(frame->code_offset >= size))
            return UACPI_STATUS_OUT_OF_BOUNDS;

        op <<= 8;
        op |= AML_READ(code, frame->code_offset++);
    }

    ctx->cur_op = uacpi_get_op_spec(op);
    if (uacpi_unlikely(ctx->cur_op->properties & UACPI_OP_PROPERTY_RESERVED))
        return UACPI_STATUS_BAD_BYTECODE;

    return UACPI_STATUS_OK;
}

static uacpi_status handle_buffer(struct execution_context *ctx)
{
    struct package_length *pkg;
    uacpi_u8 *src;
    uacpi_object *dst, *declared_size;
    uacpi_u32 buffer_size, init_size, aml_offset;
    struct op_context *op_ctx = ctx->cur_op_ctx;

    aml_offset = item_array_at(&op_ctx->items, 2)->immediate;
    src = ctx->cur_frame->method->code;
    src += aml_offset;

    pkg = &item_array_at(&op_ctx->items, 0)->pkg;
    init_size = pkg->end - aml_offset;

    // TODO: do package bounds checking at parse time
    if (uacpi_unlikely(pkg->end > ctx->cur_frame->method->size))
        return UACPI_STATUS_BAD_BYTECODE;

    declared_size = item_array_at(&op_ctx->items, 1)->obj;

    if (uacpi_unlikely(declared_size->integer > 0xE0000000)) {
        uacpi_kernel_log(
            UACPI_LOG_WARN,
            "buffer is too large (%llu), assuming corrupted bytestream\n",
            declared_size->integer
        );
        return UACPI_STATUS_BAD_BYTECODE;
    }

    if (uacpi_unlikely(declared_size->integer == 0)) {
        uacpi_kernel_log(
            UACPI_LOG_WARN, "attempted to create an empty buffer\n"
        );
        return UACPI_STATUS_BAD_BYTECODE;
    }

    buffer_size = declared_size->integer;
    if (uacpi_unlikely(init_size > buffer_size)) {
        uacpi_kernel_log(
            UACPI_LOG_WARN,
            "too many buffer initializers: %u (size is %u)\n",
            init_size, buffer_size
        );
        return UACPI_STATUS_BAD_BYTECODE;
    }

    dst = item_array_at(&op_ctx->items, 3)->obj;
    dst->buffer->data = uacpi_kernel_alloc(buffer_size);
    if (uacpi_unlikely(dst->buffer->data == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;
    dst->buffer->size = buffer_size;

    uacpi_memcpy_zerout(dst->buffer->data, src, buffer_size, init_size);
    return UACPI_STATUS_OK;
}

uacpi_status handle_string(struct execution_context *ctx)
{
    struct call_frame *frame = ctx->cur_frame;
    uacpi_object *obj;

    char *string;
    size_t length;

    obj = item_array_last(&ctx->cur_op_ctx->items)->obj;
    string = call_frame_cursor(frame);

    // TODO: sanitize string for valid UTF-8
    length = uacpi_strnlen(string, call_frame_code_bytes_left(frame));

    if (string[length++] != 0x00)
        return UACPI_STATUS_BAD_BYTECODE;

    obj->buffer->text = uacpi_kernel_alloc(length);
    if (uacpi_unlikely(obj->buffer->text == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    uacpi_memcpy(obj->buffer->text, string, length);
    obj->buffer->size = length;
    frame->code_offset += length;
    return UACPI_STATUS_OK;
}

static uacpi_status handle_package(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_package *package;
    uacpi_u32 num_elements, num_defined_elements, i;

    /*
     * Layout of items here:
     * [0] -> Package length, not interesting
     * [1] -> Immediate or integer object, depending on PackageOp/VarPackageOp
     * [2..N-2] -> AML pc+Package element pairs
     * [N-1] -> The resulting package object that we're constructing
     */
    package = item_array_last(&op_ctx->items)->obj->package;

    // 1. Detect how many elements we have, do sanity checking
    if (op_ctx->op->code == UACPI_AML_OP_VarPackageOp) {
        uacpi_object *var_num_elements;

        var_num_elements = item_array_at(&op_ctx->items, 1)->obj;
        if (uacpi_unlikely(var_num_elements->integer > 0xE0000000)) {
            uacpi_kernel_log(
                UACPI_LOG_WARN,
                "package is too large (%llu), assuming corrupted bytestream\n",
                var_num_elements->integer
            );
            return UACPI_STATUS_BAD_BYTECODE;
        }
        num_elements = var_num_elements->integer;
    } else {
        num_elements = item_array_at(&op_ctx->items, 1)->immediate;
    }

    num_defined_elements = (item_array_size(&op_ctx->items) - 3) / 2;
    if (uacpi_unlikely(num_defined_elements > num_elements)) {
        uacpi_kernel_log(
            UACPI_LOG_WARN,
            "too many package initializers: %u, truncating to %u\n",
            num_defined_elements, num_elements
        );

        num_defined_elements = num_elements;
    }

    // 2. Create every object in the package, start as uninitialized
    if (uacpi_unlikely(!uacpi_package_fill(package, num_elements)))
        return UACPI_STATUS_OUT_OF_MEMORY;

    // 3. Go through every defined object and copy it into the package
    for (i = 0; i < num_defined_elements; ++i) {
        uacpi_size base_pkg_index;
        uacpi_status ret;
        struct item *item;
        uacpi_object *obj;

        base_pkg_index = (i * 2) + 2;
        item = item_array_at(&op_ctx->items, base_pkg_index + 1);
        obj = item->obj;

        if (obj != UACPI_NULL && obj->type == UACPI_OBJECT_REFERENCE) {
            /*
             * For named objects we don't actually need the object itself, but
             * simply the path to it. Often times objects referenced by the
             * package are not defined until later so it's not possible to
             * resolve them. For uniformity and to follow the behavior of NT,
             * simply convert the name string to a path string object to be
             * resolved later when actually needed.
             */
            if (obj->flags == UACPI_REFERENCE_KIND_NAMED) {
                uacpi_object_unref(obj);
                item->obj = UACPI_NULL;
                obj = UACPI_NULL;
            } else {
                obj = uacpi_unwrap_internal_reference(obj);
            }
        }

        if (obj == UACPI_NULL) {
            uacpi_size length;
            uacpi_char *path;

            obj = uacpi_create_object(UACPI_OBJECT_STRING);
            if (uacpi_unlikely(obj == UACPI_NULL))
                return UACPI_STATUS_OUT_OF_MEMORY;

            ret = name_string_to_path(
                ctx->cur_frame,
                item_array_at(&op_ctx->items, base_pkg_index)->immediate,
                &path, &length
            );
            if (uacpi_unlikely_error(ret))
                return ret;

            obj->flags = UACPI_STRING_KIND_PATH;
            obj->buffer->text = path;
            obj->buffer->size = length;

            item->obj = obj;
            item->type = ITEM_OBJECT;
        }

        ret = uacpi_object_assign(package->objects[i], obj,
                                  UACPI_ASSIGN_BEHAVIOR_DEEP_COPY);
        if (uacpi_unlikely_error(ret))
            return ret;
    }

    return UACPI_STATUS_OK;
}

static uacpi_size buffer_field_byte_size(struct uacpi_buffer_field *field)
{
    return UACPI_ALIGN_UP(field->bit_length, 8, uacpi_u32) / 8;
}

static uacpi_size sizeof_int()
{
    return g_uacpi_rt_ctx.is_rev1 ? 4 : 8;
}

struct object_storage_as_buffer {
    void *ptr;
    uacpi_size len;
};

static uacpi_status get_object_storage(uacpi_object *obj,
                                       struct object_storage_as_buffer *out_buf,
                                       uacpi_bool include_null)
{
    switch (obj->type) {
    case UACPI_OBJECT_INTEGER:
        out_buf->len = sizeof_int();
        out_buf->ptr = &obj->integer;
        break;
    case UACPI_OBJECT_STRING:
        out_buf->len = obj->buffer->size;
        if (out_buf->len && !include_null)
            out_buf->len--;

        out_buf->ptr = obj->buffer->text;
        break;
    case UACPI_OBJECT_BUFFER:
        if (obj->buffer->size == 0) {
            out_buf->len = 0;
            break;
        }

        out_buf->len = obj->buffer->size;
        out_buf->ptr = obj->buffer->data;
        break;
    case UACPI_OBJECT_REFERENCE:
        return UACPI_STATUS_INVALID_ARGUMENT;
    default:
        return UACPI_STATUS_BAD_BYTECODE;
    }

    return UACPI_STATUS_OK;
}

struct bit_span
{
    uacpi_u8 *data;
    uacpi_u64 index;
    uacpi_u64 length;
};

static void do_rw_misaligned_buffer_field(struct bit_span *dst, struct bit_span *src)
{
    uacpi_u8 src_shift, dst_shift, bits = 0;
    uacpi_u16 dst_mask;
    uacpi_u8 *dst_ptr, *src_ptr;
    uacpi_u64 dst_count, src_count;

    dst_ptr = dst->data + (dst->index / 8);
    src_ptr = src->data + (src->index / 8);

    dst_count = dst->length;
    dst_shift = dst->index & 7;

    src_count = src->length;
    src_shift = src->index & 7;

    while (dst_count)
    {
        bits = 0;

        if (src_count) {
            bits = *src_ptr >> src_shift;

            if (src_shift && src_count > 8 - src_shift)
                bits |= *(src_ptr + 1) << (8 - src_shift);

            if (src_count < 8) {
                bits &= (1 << src_count) - 1;
                src_count = 0;
            } else {
                src_count -= 8;
                src_ptr++;
            }
        }

        dst_mask = (dst_count < 8 ? (1 << dst_count) - 1 : 0xFF) << dst_shift;
        *dst_ptr = (*dst_ptr & ~dst_mask) | ((bits << dst_shift) & dst_mask);

        if (dst_shift && dst_count > (8 - dst_shift)) {
            dst_mask >>= 8;
            *(dst_ptr + 1) &= ~dst_mask;
            *(dst_ptr + 1) |= (bits >> (8 - dst_shift)) & dst_mask;
        }

        dst_count = dst_count > 8 ? dst_count - 8 : 0;
        ++dst_ptr;
    }
}

static void do_write_misaligned_buffer_field(
    uacpi_buffer_field *field,
    struct object_storage_as_buffer src_buf
)
{
    struct bit_span src_span = {
        .length = src_buf.len * 8,
        .data = src_buf.ptr,
    };
    struct bit_span dst_span = {
        .index = field->bit_index,
        .length = field->bit_length,
        .data = field->backing->data,
    };

    do_rw_misaligned_buffer_field(&dst_span, &src_span);
}

static void write_buffer_field(uacpi_buffer_field *field,
                               struct object_storage_as_buffer src_buf)
{
    if (!(field->bit_index & 7)) {
        uacpi_u8 *dst, last_byte, tail_shift;
        uacpi_size count;

        dst = field->backing->data;
        dst += field->bit_index / 8;
        count = buffer_field_byte_size(field);

        last_byte = dst[count - 1];
        tail_shift = field->bit_length & 7;

        uacpi_memcpy_zerout(dst, src_buf.ptr, count, src_buf.len);
        if (tail_shift)
            dst[count - 1] |= (last_byte >> tail_shift) << tail_shift;

        return;
    }

    do_write_misaligned_buffer_field(field, src_buf);
}

static uacpi_u8 *buffer_index_cursor(uacpi_buffer_index *buf_idx)
{
    uacpi_u8 *out_cursor;

    out_cursor = buf_idx->buffer->data;
    out_cursor += buf_idx->idx;

    return out_cursor;
}

static void write_buffer_index(uacpi_buffer_index *buf_idx,
                               struct object_storage_as_buffer *src_buf)
{
    uacpi_memcpy_zerout(buffer_index_cursor(buf_idx), src_buf->ptr,
                        1, src_buf->len);
}

/*
 * The word "implicit cast" here is only because it's called that in
 * the specification. In reality, we just copy one buffer to another
 * because that's what NT does.
 */
static uacpi_status object_assign_with_implicit_cast(uacpi_object *dst,
                                                     uacpi_object *src)
{
    uacpi_status ret;
    struct object_storage_as_buffer src_buf;

    ret = get_object_storage(src, &src_buf, UACPI_FALSE);
    if (uacpi_unlikely_error(ret))
        return ret;

    switch (dst->type) {
    case UACPI_OBJECT_INTEGER:
    case UACPI_OBJECT_STRING:
    case UACPI_OBJECT_BUFFER: {
        struct object_storage_as_buffer dst_buf;

        ret = get_object_storage(dst, &dst_buf, UACPI_FALSE);
        if (uacpi_unlikely_error(ret))
            return ret;

        uacpi_memcpy_zerout(dst_buf.ptr, src_buf.ptr, dst_buf.len, src_buf.len);
        break;
    }

    case UACPI_OBJECT_BUFFER_FIELD:
        write_buffer_field(&dst->buffer_field, src_buf);
        break;

    case UACPI_OBJECT_BUFFER_INDEX:
        write_buffer_index(&dst->buffer_index, &src_buf);
        break;

    default:
        ret = UACPI_STATUS_BAD_BYTECODE;
        break;
    }

    return ret;
}

enum argx_or_localx {
    ARGX,
    LOCALX,
};

static uacpi_status handle_arg_or_local(
    struct execution_context *ctx,
    uacpi_size idx, enum argx_or_localx type
)
{
    uacpi_object **src;
    struct item *dst;
    enum uacpi_reference_kind kind;

    if (type == ARGX) {
        src = &ctx->cur_frame->args[idx];
        kind = UACPI_REFERENCE_KIND_ARG;
    } else {
        src = &ctx->cur_frame->locals[idx];
        kind = UACPI_REFERENCE_KIND_LOCAL;
    }

    if (*src == UACPI_NULL) {
        uacpi_object *default_value;

        default_value = uacpi_create_object(UACPI_OBJECT_UNINITIALIZED);
        if (uacpi_unlikely(default_value == UACPI_NULL))
            return UACPI_STATUS_OUT_OF_MEMORY;

        *src = uacpi_create_internal_reference(kind, default_value);
        if (uacpi_unlikely(*src == UACPI_NULL))
            return UACPI_STATUS_OUT_OF_MEMORY;

        uacpi_object_unref(default_value);
    }

    dst = item_array_last(&ctx->cur_op_ctx->items);
    dst->obj = *src;
    dst->type = ITEM_OBJECT;
    uacpi_object_ref(dst->obj);

    return UACPI_STATUS_OK;
}

static uacpi_status handle_local(struct execution_context *ctx)
{
    uacpi_size idx;
    struct op_context *op_ctx = ctx->cur_op_ctx;

    idx = op_ctx->op->code - UACPI_AML_OP_Local0Op;
    return handle_arg_or_local(ctx, idx, LOCALX);
}

static uacpi_status handle_arg(struct execution_context *ctx)
{
    uacpi_size idx;
    struct op_context *op_ctx = ctx->cur_op_ctx;

    idx = op_ctx->op->code - UACPI_AML_OP_Arg0Op;
    return handle_arg_or_local(ctx, idx, ARGX);
}

static uacpi_status handle_named_object(struct execution_context *ctx)
{
    struct uacpi_namespace_node *src;
    struct item *dst;

    src = item_array_at(&ctx->cur_op_ctx->items, 0)->node;
    dst = item_array_at(&ctx->cur_op_ctx->items, 1);

    dst->obj = src->object;
    dst->type = ITEM_OBJECT;
    uacpi_object_ref(dst->obj);

    return UACPI_STATUS_OK;
}

static uacpi_status handle_create_alias(struct execution_context *ctx)
{
    uacpi_namespace_node *src, *dst;

    src = item_array_at(&ctx->cur_op_ctx->items, 0)->node;
    dst = item_array_at(&ctx->cur_op_ctx->items, 1)->node;

    dst->object = src->object;
    uacpi_object_ref(dst->object);

    return UACPI_STATUS_OK;
}

static uacpi_status handle_create_op_region(struct execution_context *ctx)
{
    uacpi_namespace_node *node;
    uacpi_object *obj;
    uacpi_operation_region *op_region;

    node = item_array_at(&ctx->cur_op_ctx->items, 0)->node;
    obj = item_array_at(&ctx->cur_op_ctx->items, 4)->obj;
    op_region = &obj->op_region;

    op_region->space = item_array_at(&ctx->cur_op_ctx->items, 1)->immediate;
    op_region->offset = item_array_at(&ctx->cur_op_ctx->items, 2)->obj->integer;
    op_region->length = item_array_at(&ctx->cur_op_ctx->items, 3)->obj->integer;

    node->object = obj;
    uacpi_object_ref(obj);

    return UACPI_STATUS_OK;
}

static uacpi_status handle_create_field(struct execution_context *ctx)
{
    return UACPI_STATUS_OK;
}

static void truncate_number_if_needed(uacpi_object *obj)
{
    if (!g_uacpi_rt_ctx.is_rev1)
        return;

    obj->integer &= 0xFFFFFFFF;
}

static uacpi_u64 ones()
{
    return g_uacpi_rt_ctx.is_rev1 ? 0xFFFFFFFF : 0xFFFFFFFFFFFFFFFF;
}

static uacpi_status method_get_ret_target(struct execution_context *ctx,
                                          uacpi_object **out_operand)
{
    uacpi_size depth;

    // Check if we're targeting the previous call frame
    depth = call_frame_array_size(&ctx->call_stack);
    if (depth > 1) {
        struct op_context *op_ctx;
        struct call_frame *frame;

        frame = call_frame_array_at(&ctx->call_stack, depth - 2);
        depth = op_context_array_size(&frame->pending_ops);

        // Ok, no one wants the return value at call site. Discard it.
        if (!depth) {
            *out_operand = UACPI_NULL;
            return UACPI_STATUS_OK;
        }

        op_ctx = op_context_array_at(&frame->pending_ops, depth - 1);
        *out_operand = item_array_last(&op_ctx->items)->obj;
        return UACPI_STATUS_OK;
    }

    return UACPI_STATUS_NOT_FOUND;
}

static uacpi_status method_get_ret_object(struct execution_context *ctx,
                                          uacpi_object **out_obj)
{
    uacpi_status ret;

    ret = method_get_ret_target(ctx, out_obj);
    if (ret == UACPI_STATUS_NOT_FOUND) {
        *out_obj = ctx->ret;
        return UACPI_STATUS_OK;
    }
    if (ret != UACPI_STATUS_OK || *out_obj == UACPI_NULL)
        return ret;

    *out_obj = uacpi_unwrap_internal_reference(*out_obj);
    return UACPI_STATUS_OK;
}

static struct code_block *find_last_block(struct code_block_array *blocks,
                                          enum code_block_type type)
{
    uacpi_size i;

    i = code_block_array_size(blocks);
    while (i-- > 0) {
        struct code_block *block;

        block = code_block_array_at(blocks, i);
        if (block->type == type)
            return block;
    }

    return UACPI_NULL;
}

static void update_scope(struct call_frame *frame)
{
    struct code_block *block;

    block = find_last_block(&frame->code_blocks, CODE_BLOCK_SCOPE);
    if (block == UACPI_NULL) {
        frame->cur_scope = uacpi_namespace_root();
        return;
    }

    frame->cur_scope = block->node;
}

static uacpi_status begin_block_execution(struct execution_context *ctx)
{
    struct call_frame *cur_frame = ctx->cur_frame;
    struct op_context *op_ctx = ctx->cur_op_ctx;
    struct package_length *pkg;
    struct code_block *block;

    block = code_block_array_alloc(&cur_frame->code_blocks);
    if (uacpi_unlikely(block == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    switch (op_ctx->op->code) {
    case UACPI_AML_OP_IfOp:
        block->type = CODE_BLOCK_IF;
        break;
    case UACPI_AML_OP_ElseOp:
        block->type = CODE_BLOCK_ELSE;
        break;
    case UACPI_AML_OP_WhileOp:
        block->type = CODE_BLOCK_WHILE;
        break;
    case UACPI_AML_OP_ScopeOp:
    case UACPI_AML_OP_DeviceOp:
    case UACPI_AML_OP_ProcessorOp:
    case UACPI_AML_OP_PowerResOp:
    case UACPI_AML_OP_ThermalZoneOp:
        block->type = CODE_BLOCK_SCOPE;
        block->node = item_array_at(&op_ctx->items, 1)->node;
        break;
    default:
        code_block_array_pop(&cur_frame->code_blocks);
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    pkg = &item_array_at(&op_ctx->items, 0)->pkg;

    // -1 because we want to re-evaluate at the start of the op next time
    block->begin = pkg->begin - 1;
    block->end = pkg->end;
    ctx->cur_block = block;

    cur_frame->last_while = find_last_block(&cur_frame->code_blocks,
                                            CODE_BLOCK_WHILE);
    update_scope(cur_frame);
    return UACPI_STATUS_OK;
}

static void frame_reset_post_end_block(struct execution_context *ctx,
                                       enum code_block_type type)
{
    struct call_frame *frame = ctx->cur_frame;
    code_block_array_pop(&frame->code_blocks);
    ctx->cur_block = code_block_array_last(&frame->code_blocks);

    if (type == CODE_BLOCK_WHILE) {
        frame->last_while = find_last_block(&frame->code_blocks, type);
    } else if (type == CODE_BLOCK_SCOPE) {
        update_scope(frame);
    }
}

static void debug_store_no_recurse(const char *prefix, uacpi_object *src)
{
    switch (src->type) {
    case UACPI_OBJECT_UNINITIALIZED:
        uacpi_kernel_log(UACPI_LOG_INFO, "%s Uninitialized\n", prefix);
        break;
    case UACPI_OBJECT_STRING:
        uacpi_kernel_log(UACPI_LOG_INFO, "%s String => \"%s\"\n",
                         prefix, src->buffer->text);
        break;
    case UACPI_OBJECT_INTEGER:
        if (g_uacpi_rt_ctx.is_rev1) {
            uacpi_kernel_log(UACPI_LOG_INFO, "%s Integer => 0x%08llX\n",
                             prefix, src->integer);
        } else {
            uacpi_kernel_log(UACPI_LOG_INFO, "%s Integer => 0x%016llX\n",
                             prefix, src->integer);
        }
        break;
    case UACPI_OBJECT_REFERENCE:
        uacpi_kernel_log(
            UACPI_LOG_INFO, "%s Reference @%p => %p\n",
            prefix, src, src->inner_object
        );
        break;
    case UACPI_OBJECT_PACKAGE:
        uacpi_kernel_log(
            UACPI_LOG_INFO, "%s Package @%p (%p) (%zu elements)\n",
            prefix, src, src->package, src->package->count
        );
        break;
    case UACPI_OBJECT_BUFFER:
        uacpi_kernel_log(
            UACPI_LOG_INFO, "%s Buffer @%p (%p) (%zu bytes)\n",
            prefix, src, src->buffer, src->buffer->size
        );
        break;
    case UACPI_OBJECT_OPERATION_REGION:
        uacpi_kernel_log(
            UACPI_LOG_INFO,
            "%s OperationRegion (ASID %d) 0x%016llX -> 0x%016llX\n",
            prefix, src->op_region.space, src->op_region.offset,
            src->op_region.offset + src->op_region.length
        );
        break;
    case UACPI_OBJECT_POWER_RESOURCE:
        uacpi_kernel_log(
            UACPI_LOG_INFO, "%s Power Resource %d %d\n",
            prefix, src->power_resource.system_level,
            src->power_resource.resource_order
        );
        break;
    case UACPI_OBJECT_PROCESSOR:
        uacpi_kernel_log(
            UACPI_LOG_INFO, "%s Processor[%d] 0x%08X (%d)\n",
            prefix, src->processor.id, src->processor.block_address,
            src->processor.block_length
        );
        break;
    case UACPI_OBJECT_BUFFER_INDEX:
        uacpi_kernel_log(
            UACPI_LOG_INFO, "%s Buffer Index %p[%zu] => 0x%02X\n",
            prefix, src->buffer_index.buffer->data, src->buffer_index.idx,
            *buffer_index_cursor(&src->buffer_index)
        );
        break;
    case UACPI_OBJECT_MUTEX:
        uacpi_kernel_log(
            UACPI_LOG_INFO,
            "%s Mutex @%p (%p => %p) sync level %d (owned by %p)\n",
            prefix, src, src->mutex, src->mutex->handle,
            src->mutex->sync_level, src->mutex->owner
        );
        break;
    default:
        uacpi_kernel_log(
            UACPI_LOG_INFO, "%s %s @%p\n",
            prefix, uacpi_object_type_to_string(src->type), src
        );
    }
}

static uacpi_status debug_store(uacpi_object *src)
{
    src = uacpi_unwrap_internal_reference(src);

    debug_store_no_recurse("[AML DEBUG]", src);

    if (src->type == UACPI_OBJECT_PACKAGE) {
        uacpi_package *pkg = src->package;
        uacpi_size i;

        for (i = 0; i < pkg->count; ++i) {
            uacpi_object *obj = pkg->objects[i];
            if (obj->type == UACPI_OBJECT_REFERENCE &&
                obj->flags == UACPI_REFERENCE_KIND_PKG_INDEX)
                obj = obj->inner_object;

            debug_store_no_recurse("Element:", obj);
        }
    }

    return UACPI_STATUS_OK;
}

/*
 * NOTE: this function returns the parent object
 */
uacpi_object *reference_unwind(uacpi_object *obj)
{
    uacpi_object *parent = obj;

    while (obj) {
        if (obj->type != UACPI_OBJECT_REFERENCE)
            return parent;

        parent = obj;
        obj = parent->inner_object;
    }

    // This should be unreachable
    return UACPI_NULL;
}

/*
 * Object implicit dereferencing [Store(..., obj)/Increment(obj),...] behavior:
 * RefOf -> the bottom-most referenced object
 * LocalX/ArgX -> object stored at LocalX if LocalX is not a reference,
 *                otherwise goto RefOf case.
 * NAME -> object stored at NAME
 */
static uacpi_object *object_deref_implicit(uacpi_object *obj)
{
    if (obj->flags != UACPI_REFERENCE_KIND_REFOF) {
        if (obj->flags == UACPI_REFERENCE_KIND_NAMED ||
            obj->inner_object->type != UACPI_OBJECT_REFERENCE)
            return obj->inner_object;

        obj = obj->inner_object;
    }

    return reference_unwind(obj)->inner_object;
}

static void object_replace_child(uacpi_object *parent, uacpi_object *new_child)
{
    uacpi_object_detach_child(parent);
    uacpi_object_attach_child(parent, new_child);
}

/*
 * Breakdown of what happens here:
 *
 * CopyObject(..., Obj) where Obj is:
 * 1. LocalX -> Overwrite LocalX.
 * 2. NAME -> Overwrite NAME.
 * 3. ArgX -> Overwrite ArgX unless ArgX is a reference, in that case
 *            overwrite the referenced object.
 * 4. RefOf -> Not allowed here.
 * 5. Index -> Overwrite Object stored at the index.
 */
 static uacpi_status copy_object_to_reference(uacpi_object *dst,
                                              uacpi_object *src)
{
    uacpi_status ret;
    uacpi_object *src_obj, *new_obj;
    uacpi_u32 refs;

    switch (dst->flags) {
    case UACPI_REFERENCE_KIND_ARG: {
        uacpi_object *referenced_obj;

        referenced_obj = uacpi_unwrap_internal_reference(dst);
        if (referenced_obj->type == UACPI_OBJECT_REFERENCE) {
            dst = reference_unwind(referenced_obj);
            break;
        }

        // FALLTHROUGH intended here
    }
    case UACPI_REFERENCE_KIND_LOCAL:
    case UACPI_REFERENCE_KIND_PKG_INDEX:
    case UACPI_REFERENCE_KIND_NAMED:
        break;
    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    src_obj = uacpi_unwrap_internal_reference(src);

    new_obj = uacpi_create_object(UACPI_OBJECT_UNINITIALIZED);
    if (uacpi_unlikely(new_obj == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    ret = uacpi_object_assign(new_obj, src_obj,
                              UACPI_ASSIGN_BEHAVIOR_DEEP_COPY);
    if (uacpi_unlikely_error(ret))
        return ret;

    object_replace_child(dst, new_obj);
    uacpi_object_unref(new_obj);

    return UACPI_STATUS_OK;
}

/*
 * if Store(..., Obj) where Obj is:
 * 1. LocalX/Index -> OVERWRITE unless the object is a reference, in that
 *                    case store to the referenced object _with_ implicit
 *                    cast.
 * 2. ArgX -> OVERWRITE unless the object is a reference, in that
 *            case OVERWRITE the referenced object.
 * 3. NAME -> Store with implicit cast.
 * 4. RefOf -> Not allowed here.
 */
static uacpi_status store_to_reference(uacpi_object *dst,
                                       uacpi_object *src)
{
    uacpi_object *src_obj;
    uacpi_bool overwrite = UACPI_FALSE;

    switch (dst->flags) {
    case UACPI_REFERENCE_KIND_LOCAL:
    case UACPI_REFERENCE_KIND_ARG:
    case UACPI_REFERENCE_KIND_PKG_INDEX: {
        uacpi_object *referenced_obj;

        if (dst->flags == UACPI_REFERENCE_KIND_PKG_INDEX)
            referenced_obj = dst->inner_object;
        else
            referenced_obj = uacpi_unwrap_internal_reference(dst);

        if (referenced_obj->type == UACPI_OBJECT_REFERENCE) {
            overwrite = dst->flags == UACPI_REFERENCE_KIND_ARG;
            dst = reference_unwind(referenced_obj);
            break;
        }

        overwrite = UACPI_TRUE;
        break;
    }
    case UACPI_REFERENCE_KIND_NAMED:
        dst = reference_unwind(dst);
        break;
    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    src_obj = uacpi_unwrap_internal_reference(src);
    overwrite |= dst->inner_object->type == UACPI_OBJECT_UNINITIALIZED;

    if (overwrite) {
        uacpi_status ret;
        uacpi_object *new_obj;

        new_obj = uacpi_create_object(UACPI_OBJECT_UNINITIALIZED);
        if (uacpi_unlikely(new_obj == UACPI_NULL))
            return UACPI_STATUS_OUT_OF_MEMORY;

        ret = uacpi_object_assign(new_obj, src_obj,
                                  UACPI_ASSIGN_BEHAVIOR_DEEP_COPY);
        if (uacpi_unlikely_error(ret)) {
            uacpi_object_unref(new_obj);
            return ret;
        }

        object_replace_child(dst, new_obj);
        uacpi_object_unref(new_obj);
        return UACPI_STATUS_OK;
    }

    return object_assign_with_implicit_cast(dst->inner_object, src_obj);
}

static uacpi_status handle_inc_dec(struct execution_context *ctx)
{
    uacpi_object *obj;
    struct op_context *op_ctx = ctx->cur_op_ctx;

    obj = item_array_at(&op_ctx->items, 0)->obj;

    if (op_ctx->op->code == UACPI_AML_OP_IncrementOp)
        obj->integer++;
    else
        obj->integer--;

    return UACPI_STATUS_OK;
}

static uacpi_status handle_ref_or_deref_of(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_object *dst, *src;

    src = item_array_at(&op_ctx->items, 0)->obj;

    if (op_ctx->op->code == UACPI_AML_OP_CondRefOfOp)
        dst = item_array_at(&op_ctx->items, 2)->obj;
    else
        dst = item_array_at(&op_ctx->items, 1)->obj;

    if (op_ctx->op->code == UACPI_AML_OP_DerefOfOp) {
        uacpi_bool was_a_reference = UACPI_FALSE;

        if (src->type == UACPI_OBJECT_REFERENCE) {
            was_a_reference = UACPI_TRUE;

            /*
             * Explicit dereferencing [DerefOf] behavior:
             * Simply grabs the bottom-most object that is not a reference.
             * This mimics the behavior of NT Acpi.sys: any DerfOf fetches
             * the bottom-most reference. Note that this is different from
             * ACPICA where DerefOf dereferences one level.
             */
            src = reference_unwind(src)->inner_object;
        }

        if (src->type == UACPI_OBJECT_BUFFER_INDEX) {
            uacpi_buffer_index *buf_idx = &src->buffer_index;

            dst->type = UACPI_OBJECT_INTEGER;
            uacpi_memcpy_zerout(
                &dst->integer, buffer_index_cursor(buf_idx),
                sizeof(dst->integer), 1
            );
            return UACPI_STATUS_OK;
        }

        if (!was_a_reference) {
            uacpi_kernel_log(
                UACPI_LOG_WARN,
                "Invalid DerefOf argument: %s, expected a reference\n",
                uacpi_object_type_to_string(src->type)
            );
            return UACPI_STATUS_BAD_BYTECODE;
        }

        return uacpi_object_assign(dst, src,
                                   UACPI_ASSIGN_BEHAVIOR_SHALLOW_COPY);
    }

    dst->type = UACPI_OBJECT_REFERENCE;
    dst->inner_object = src;
    uacpi_object_ref(src);
    return UACPI_STATUS_OK;
}

static void do_binary_math(uacpi_object *arg0, uacpi_object *arg1,
                           uacpi_object *tgt0, uacpi_object *tgt1,
                           uacpi_aml_op op)
{
    uacpi_u64 lhs, rhs, res;
    uacpi_bool should_negate = UACPI_FALSE;

    lhs = arg0->integer;
    rhs = arg1->integer;

    switch (op)
    {
    case UACPI_AML_OP_AddOp:
        res = lhs + rhs;
        break;
    case UACPI_AML_OP_SubtractOp:
        res = lhs - rhs;
        break;
    case UACPI_AML_OP_MultiplyOp:
        res = lhs * rhs;
        break;
    case UACPI_AML_OP_ShiftLeftOp:
    case UACPI_AML_OP_ShiftRightOp:
        if (rhs <= (g_uacpi_rt_ctx.is_rev1 ? 31 : 63)) {
            if (op == UACPI_AML_OP_ShiftLeftOp)
                res = lhs << rhs;
            else
                res = lhs >> rhs;
        } else {
            res = 0;
        }
        break;
    case UACPI_AML_OP_NandOp:
        should_negate = UACPI_TRUE;
    case UACPI_AML_OP_AndOp:
        res = rhs & lhs;
        break;
    case UACPI_AML_OP_NorOp:
        should_negate = UACPI_TRUE;
    case UACPI_AML_OP_OrOp:
        res = rhs | lhs;
        break;
    case UACPI_AML_OP_XorOp:
        res = rhs ^ lhs;
        break;
    case UACPI_AML_OP_DivideOp:
        if (uacpi_likely(rhs > 0)) {
            tgt1->integer = lhs / rhs;
        } else {
            uacpi_kernel_log(UACPI_LOG_WARN, "Attempted division by zero!\n");
            tgt1->integer = 0;
        }
        // FALLTHROUGH intended here
    case UACPI_AML_OP_ModOp:
        res = lhs % rhs;
        break;
    default:
        break;
    }

    if (should_negate)
        res = ~res;

    tgt0->integer = res;
}

static uacpi_status handle_binary_math(struct execution_context *ctx)
{
    uacpi_object *arg0, *arg1, *tgt0, *tgt1;
    struct item_array *items = &ctx->cur_op_ctx->items;
    uacpi_aml_op op = ctx->cur_op_ctx->op->code;

    arg0 = item_array_at(items, 0)->obj;
    arg1 = item_array_at(items, 1)->obj;

    if (op == UACPI_AML_OP_DivideOp) {
        tgt0 = item_array_at(items, 4)->obj;
        tgt1 = item_array_at(items, 5)->obj;
    } else {
        tgt0 = item_array_at(items, 3)->obj;
        tgt1 = UACPI_NULL;
    }

    do_binary_math(arg0, arg1, tgt0, tgt1, op);
    return UACPI_STATUS_OK;
}

static uacpi_status handle_unary_math(struct execution_context *ctx)
{
    uacpi_object *arg, *tgt;
    struct item_array *items = &ctx->cur_op_ctx->items;
    uacpi_aml_op op = ctx->cur_op_ctx->op->code;

    arg = item_array_at(items, 0)->obj;
    tgt = item_array_at(items, 2)->obj;

    switch (op) {
    case UACPI_AML_OP_NotOp:
        tgt->integer = ~arg->integer;
        truncate_number_if_needed(tgt);
        break;
    case UACPI_AML_OP_FindSetRightBitOp:
        tgt->integer = uacpi_bit_scan_forward(arg->integer);
        break;
    case UACPI_AML_OP_FindSetLeftBitOp:
        tgt->integer = uacpi_bit_scan_backward(arg->integer);
        break;
    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    return UACPI_STATUS_OK;
}

static uacpi_status ensure_valid_idx(uacpi_size idx, uacpi_size src_size)
{
    if (uacpi_likely(idx < src_size))
        return UACPI_STATUS_OK;

    uacpi_kernel_log(
        UACPI_LOG_WARN,
        "Invalid index %zu, object has %zu elements\n",
        idx, src_size
    );
    return UACPI_STATUS_BAD_BYTECODE;
}

static uacpi_status handle_index(struct execution_context *ctx)
{
    uacpi_status ret;
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_object *src;
    struct item *dst;
    uacpi_size idx;

    src = item_array_at(&op_ctx->items, 0)->obj;
    idx = item_array_at(&op_ctx->items, 1)->obj->integer;
    dst = item_array_at(&op_ctx->items, 3);

    switch (src->type) {
    case UACPI_OBJECT_BUFFER:
    case UACPI_OBJECT_STRING: {
        uacpi_buffer_index *buf_idx;
        struct object_storage_as_buffer buf;
        get_object_storage(src, &buf, UACPI_FALSE);

        ret = ensure_valid_idx(idx, buf.len);
        if (uacpi_unlikely_error(ret))
            return ret;

        dst->type = ITEM_OBJECT;
        dst->obj = uacpi_create_object(UACPI_OBJECT_BUFFER_INDEX);
        if (uacpi_unlikely(dst->obj == UACPI_NULL))
            return UACPI_STATUS_OUT_OF_MEMORY;

        buf_idx = &dst->obj->buffer_index;
        buf_idx->idx = idx;
        buf_idx->buffer = src->buffer;
        uacpi_shareable_ref(buf_idx->buffer);

        break;
    }
    case UACPI_OBJECT_PACKAGE: {
        uacpi_package *pkg = src->package;
        uacpi_object *obj;

        ret = ensure_valid_idx(idx, pkg->count);
        if (uacpi_unlikely_error(ret))
            return ret;

        /*
         * Lazily transform the package element into an internal reference
         * to itself of type PKG_INDEX. This is needed to support stuff like
         * CopyObject(..., Index(pkg, X)) where the new object must be
         * propagated to anyone else with a currently alive index object.
         *
         * Sidenote: Yes, IndexOp is not a SimpleName, so technically it is
         *           illegal to CopyObject to it. However, yet again we fall
         *           victim to the NT ACPI driver implementation, which allows
         *           it just fine.
         */
        obj = pkg->objects[idx];
        if (obj->type != UACPI_OBJECT_REFERENCE ||
            obj->flags != UACPI_REFERENCE_KIND_PKG_INDEX) {

            obj = uacpi_create_internal_reference(
                UACPI_REFERENCE_KIND_PKG_INDEX, obj
            );
            if (uacpi_unlikely(obj == UACPI_NULL))
                return UACPI_STATUS_OUT_OF_MEMORY;

            pkg->objects[idx] = obj;
            uacpi_object_unref(obj->inner_object);
        }

        dst->obj = obj;
        dst->type = ITEM_OBJECT;
        uacpi_object_ref(dst->obj);
        break;
    }
    default:
        uacpi_kernel_log(
            UACPI_LOG_WARN,
            "Invalid argument for Index: %s, "
            "expected String/Buffer/Package\n",
            uacpi_object_type_to_string(src->type)
        );
        return UACPI_STATUS_BAD_BYTECODE;
    }

    return UACPI_STATUS_OK;
}

static uacpi_u64 object_to_integer(const uacpi_object *obj,
                                   uacpi_size max_buffer_bytes)
{
    uacpi_u64 dst;

    switch (obj->type) {
    case UACPI_OBJECT_INTEGER:
        dst = obj->integer;
        break;
    case UACPI_OBJECT_BUFFER: {
        uacpi_size bytes;
        bytes = UACPI_MIN(max_buffer_bytes, obj->buffer->size);
        uacpi_memcpy_zerout(&dst, obj->buffer->data, sizeof(dst), bytes);
        break;
    }
    case UACPI_OBJECT_STRING:
        dst = uacpi_strtoull(obj->buffer->text, UACPI_NULL, 0);
        break;
    default:
        dst = 0;
        break;
    }

    return dst;
}

static uacpi_status integer_to_string(
    uacpi_u64 integer, uacpi_buffer *str, uacpi_bool is_hex
)
{
    int repr_len;
    uacpi_char int_buf[21];
    uacpi_size final_size;

    repr_len = uacpi_snprintf(
        int_buf, sizeof(int_buf),
        is_hex ? "%"UACPI_PRIX64 : "%"UACPI_PRIu64,
        integer
    );
    if (uacpi_unlikely(repr_len < 0))
        return UACPI_STATUS_INVALID_ARGUMENT;

    // 0x prefix + repr + \0
    final_size = (is_hex ? 2 : 0) + repr_len + 1;

    str->data = uacpi_kernel_alloc(final_size);
    if (uacpi_unlikely(str->data == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    if (is_hex) {
        str->text[0] = '0';
        str->text[1] = 'x';
    }
    uacpi_memcpy(str->text + (is_hex ? 2 : 0), int_buf, repr_len + 1);
    str->size = final_size;

    return UACPI_STATUS_OK;
}

static uacpi_status buffer_to_string(
    uacpi_buffer *buf, uacpi_buffer *str, uacpi_bool is_hex
)
{
    int repr_len;
    uacpi_char int_buf[5];
    uacpi_size i, final_size;
    uacpi_char *cursor;

    if (is_hex) {
        final_size = 4 * buf->size;
    } else {
        final_size = 0;

        for (i = 0; i < buf->size; ++i) {
            uacpi_u8 value = ((uacpi_u8*)buf->data)[i];

            if (value < 10)
                final_size += 1;
            else if (value < 100)
                final_size += 2;
            else
                final_size += 3;
        }
    }

    // Comma for every value but one
    final_size += buf->size - 1;

    // Null terminator
    final_size += 1;

    str->data = uacpi_kernel_alloc(final_size);
    if (uacpi_unlikely(str->data == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    cursor = str->data;

    for (i = 0; i < buf->size; ++i) {
        repr_len = uacpi_snprintf(
            int_buf, sizeof(int_buf),
            is_hex ? "0x%02X" : "%d",
            ((uacpi_u8*)buf->data)[i]
        );
        if (uacpi_unlikely(repr_len < 0)) {
            uacpi_kernel_free(str->data);
            str->data = UACPI_NULL;
            return UACPI_STATUS_INVALID_ARGUMENT;
        }

        uacpi_memcpy(cursor, int_buf, repr_len + 1);
        cursor += repr_len;

        if (i != buf->size - 1)
            *cursor++ = ',';
    }

    str->size = final_size;
    return UACPI_STATUS_OK;
}

static uacpi_status do_make_empty_object(uacpi_buffer *buf,
                                         uacpi_bool is_string)
{
    buf->text = uacpi_kernel_calloc(1, sizeof(uacpi_char));
    if (uacpi_unlikely(buf->text == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    if (is_string)
        buf->size = sizeof(uacpi_char);

    return UACPI_STATUS_OK;
}

static uacpi_status make_null_string(uacpi_buffer *buf)
{
    return do_make_empty_object(buf, UACPI_TRUE);
}

static uacpi_status make_null_buffer(uacpi_buffer *buf)
{
    /*
     * Allocate at least 1 byte just to be safe,
     * even for empty buffers. We still set the
     * size to 0 though.
     */
    return do_make_empty_object(buf, UACPI_FALSE);
}

static uacpi_status handle_to(struct execution_context *ctx)
{
    uacpi_status ret = UACPI_STATUS_OK;
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_object *src, *dst;

    src = item_array_at(&op_ctx->items, 0)->obj;
    dst = item_array_at(&op_ctx->items, 2)->obj;

    switch (op_ctx->op->code) {
    case UACPI_AML_OP_ToIntegerOp:
        // NT always takes the first 8 bytes, even for revision 1
        dst->integer = object_to_integer(src, 8);
        break;

    case UACPI_AML_OP_ToHexStringOp:
    case UACPI_AML_OP_ToDecimalStringOp: {
        uacpi_bool is_hex = op_ctx->op->code == UACPI_AML_OP_ToHexStringOp;

        if (src->type == UACPI_OBJECT_INTEGER) {
            ret = integer_to_string(src->integer, dst->buffer, is_hex);
            break;
        } else if (src->type == UACPI_OBJECT_BUFFER) {
            if (uacpi_unlikely(src->buffer->size == 0))
                return make_null_string(dst->buffer);

            ret = buffer_to_string(src->buffer, dst->buffer, is_hex);
            break;
        }
        // FALLTHROUGH for string -> string conversion
    }
    case UACPI_AML_OP_ToBufferOp: {
        struct object_storage_as_buffer buf;
        uacpi_u8 *dst_buf;

        ret = get_object_storage(src, &buf, UACPI_TRUE);
        if (uacpi_unlikely_error(ret))
            return ret;

        if (uacpi_unlikely(buf.len == 0))
            return make_null_buffer(dst->buffer);

        dst_buf = uacpi_kernel_alloc(buf.len);
        if (uacpi_unlikely(dst_buf == UACPI_NULL))
            return UACPI_STATUS_OUT_OF_MEMORY;

        uacpi_memcpy(dst_buf, buf.ptr, buf.len);
        dst->buffer->data = dst_buf;
        dst->buffer->size = buf.len;
        break;
    }

    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    return ret;
}

static uacpi_status handle_to_string(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_buffer *src_buf, *dst_buf;
    uacpi_size req_len, len;

    src_buf = item_array_at(&op_ctx->items, 0)->obj->buffer;
    req_len = item_array_at(&op_ctx->items, 1)->obj->integer;
    dst_buf = item_array_at(&op_ctx->items, 3)->obj->buffer;

    len = UACPI_MIN(req_len, src_buf->size);
    if (uacpi_unlikely(len == 0))
        return make_null_string(dst_buf);

    len = uacpi_strnlen(src_buf->text, len);

    dst_buf->text = uacpi_kernel_alloc(len + 1);
    if (uacpi_unlikely(dst_buf->text == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    uacpi_memcpy(dst_buf->text, src_buf->data, len);
    dst_buf->text[len] = '\0';
    dst_buf->size = len + 1;

    return UACPI_STATUS_OK;
}

static uacpi_status handle_mid(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_object *src, *dst;
    struct object_storage_as_buffer src_buf;
    uacpi_buffer *dst_buf;
    uacpi_size idx, len, buf_size;
    uacpi_bool is_string;

    src = item_array_at(&op_ctx->items, 0)->obj;
    if (uacpi_unlikely(src->type != UACPI_OBJECT_STRING &&
                       src->type != UACPI_OBJECT_BUFFER)) {
        uacpi_kernel_log(
            UACPI_LOG_WARN,
            "Invalid argument for Mid: %s, expected String/Buffer\n",
            uacpi_object_type_to_string(src->type)
        );
        return UACPI_STATUS_BAD_BYTECODE;
    }

    idx = item_array_at(&op_ctx->items, 1)->obj->integer;
    len = item_array_at(&op_ctx->items, 2)->obj->integer;
    dst = item_array_at(&op_ctx->items, 4)->obj;
    dst_buf = dst->buffer;

    is_string = src->type == UACPI_OBJECT_STRING;
    get_object_storage(src, &src_buf, UACPI_FALSE);

    if (uacpi_unlikely(src_buf.len == 0 || idx >= src_buf.len)) {
        if (src->type == UACPI_OBJECT_STRING) {
            dst->type = UACPI_OBJECT_STRING;
            return make_null_string(dst_buf);
        }

        return make_null_buffer(dst_buf);
    }

    // Guaranteed to be at least 1 here
    len = UACPI_MIN(len, src_buf.len - idx);

    dst_buf->data = uacpi_kernel_alloc(len + is_string);
    if (uacpi_unlikely(dst_buf->data == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    uacpi_memcpy(dst_buf->data, (uacpi_u8*)src_buf.ptr + idx, len);
    dst_buf->size = len;

    if (is_string) {
        dst_buf->text[dst_buf->size++] = '\0';
        dst->type = UACPI_OBJECT_STRING;
    }

    return UACPI_STATUS_OK;
}

static uacpi_status handle_concatenate(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_object *arg0, *arg1, *dst;
    uacpi_u8 *dst_buf;
    uacpi_size buf_size = 0;

    arg0 = item_array_at(&op_ctx->items, 0)->obj;
    arg1 = item_array_at(&op_ctx->items, 1)->obj;
    dst = item_array_at(&op_ctx->items, 3)->obj;

    switch (arg0->type) {
    case UACPI_OBJECT_INTEGER: {
        uacpi_u64 arg1_as_int;
        uacpi_size int_size;

        int_size = sizeof_int();
        buf_size = int_size * 2;

        dst_buf = uacpi_kernel_alloc(buf_size);
        if (uacpi_unlikely(dst_buf == UACPI_NULL))
            return UACPI_STATUS_OUT_OF_MEMORY;

        arg1_as_int = object_to_integer(arg1, 8);

        uacpi_memcpy(dst_buf, &arg0->integer, int_size);
        uacpi_memcpy(dst_buf+ int_size, &arg1_as_int, int_size);
        break;
    }
    case UACPI_OBJECT_BUFFER: {
        uacpi_buffer *arg0_buf = arg0->buffer;
        struct object_storage_as_buffer arg1_buf;

        get_object_storage(arg1, &arg1_buf, UACPI_TRUE);
        buf_size = arg0_buf->size + arg1_buf.len;

        dst_buf = uacpi_kernel_alloc(buf_size);
        if (uacpi_unlikely(dst_buf == UACPI_NULL))
            return UACPI_STATUS_OUT_OF_MEMORY;

        uacpi_memcpy(dst_buf, arg0_buf->data, arg0_buf->size);
        uacpi_memcpy(dst_buf + arg0_buf->size, arg1_buf.ptr, arg1_buf.len);
        break;
    }
    case UACPI_OBJECT_STRING: {
        char int_buf[17];
        void *arg1_ptr;
        uacpi_size arg0_size, arg1_size;
        uacpi_buffer *arg0_buf = arg0->buffer;

        switch (arg1->type) {
        case UACPI_OBJECT_INTEGER: {
            int ret;
            ret = uacpi_snprintf(int_buf, sizeof(int_buf), "%"UACPI_PRIx64,
                                 arg1->integer);
            if (ret < 0)
                return UACPI_STATUS_INVALID_ARGUMENT;

            arg1_ptr = int_buf;
            arg1_size = ret + 1;
            break;
        }
        case UACPI_OBJECT_STRING:
            arg1_ptr = arg1->buffer->data;
            arg1_size = arg1->buffer->size;
            break;
        case UACPI_OBJECT_BUFFER:
        default:
            // NT doesn't support this, so we don't as well
            return UACPI_STATUS_INVALID_ARGUMENT;
        }

        arg0_size = arg0_buf->size ? arg0_buf->size - 1 : arg0_buf->size;
        buf_size = arg0_size + arg1_size;

        dst_buf = uacpi_kernel_alloc(buf_size);
        if (uacpi_unlikely(dst_buf == UACPI_NULL))
            return UACPI_STATUS_OUT_OF_MEMORY;

        uacpi_memcpy(dst_buf, arg0_buf->data, arg0_size);
        uacpi_memcpy(dst_buf + arg0_size, arg1_ptr, arg1_size);
        dst->type = UACPI_OBJECT_STRING;
        break;
    }
    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    dst->buffer->data = dst_buf;
    dst->buffer->size = buf_size;
    return UACPI_STATUS_OK;
}

static uacpi_status handle_sizeof(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_object *src, *dst;

    src = item_array_at(&op_ctx->items, 0)->obj;
    dst = item_array_at(&op_ctx->items, 1)->obj;

    if (uacpi_likely(src->type == UACPI_OBJECT_REFERENCE))
        src = reference_unwind(src)->inner_object;

    switch (src->type) {
    case UACPI_OBJECT_STRING:
    case UACPI_OBJECT_BUFFER: {
        struct object_storage_as_buffer buf;
        get_object_storage(src, &buf, UACPI_FALSE);

        dst->integer = buf.len;
        break;
    }

    case UACPI_OBJECT_PACKAGE:
        dst->integer = src->package->count;
        break;

    default:
        uacpi_kernel_log(
            UACPI_LOG_WARN,
            "Invalid argument for Sizeof: %s, "
            "expected String/Buffer/Package\n",
            uacpi_object_type_to_string(src->type)
        );
        return UACPI_STATUS_BAD_BYTECODE;
    }

    return UACPI_STATUS_OK;
}

static uacpi_status handle_object_type(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_object *src, *dst;

    src = item_array_at(&op_ctx->items, 0)->obj;
    dst = item_array_at(&op_ctx->items, 1)->obj;

    if (uacpi_likely(src->type == UACPI_OBJECT_REFERENCE))
        src = reference_unwind(src)->inner_object;

    dst->integer = src->type;
    if (dst->integer == UACPI_OBJECT_BUFFER_INDEX)
        dst->integer = UACPI_OBJECT_BUFFER_FIELD;

    return UACPI_STATUS_OK;
}

static uacpi_status handle_timer(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_object *dst;

    dst = item_array_at(&op_ctx->items, 0)->obj;
    dst->integer = uacpi_kernel_get_ticks();

    return UACPI_STATUS_OK;
}

static uacpi_status handle_logical_not(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_object *src, *dst;

    src = item_array_at(&op_ctx->items, 0)->obj;
    dst = item_array_at(&op_ctx->items, 1)->obj;

    dst->type = UACPI_OBJECT_INTEGER;
    dst->integer = src->integer ? 0 : ones();

    return UACPI_STATUS_OK;
}

static uacpi_bool handle_logical_equality(uacpi_object *lhs, uacpi_object *rhs)
{
    uacpi_bool res = UACPI_FALSE;

    if (lhs->type == UACPI_OBJECT_STRING || lhs->type == UACPI_OBJECT_BUFFER) {
        res = lhs->buffer->size == rhs->buffer->size;

        if (res && lhs->buffer->size) {
            res = uacpi_memcmp(
                lhs->buffer->data,
                rhs->buffer->data,
                lhs->buffer->size
            ) == 0;
        }
    } else if (lhs->type == UACPI_OBJECT_INTEGER) {
        res = lhs->integer == rhs->integer;
    }

    return res;
}

static uacpi_bool handle_logical_less_or_greater(
    uacpi_aml_op op, uacpi_object *lhs, uacpi_object *rhs
)
{
    if (lhs->type == UACPI_OBJECT_STRING || lhs->type == UACPI_OBJECT_BUFFER) {
        int res;
        uacpi_buffer *lhs_buf, *rhs_buf;

        lhs_buf = lhs->buffer;
        rhs_buf = rhs->buffer;

        res = uacpi_memcmp(lhs_buf->data, rhs_buf->data,
                           UACPI_MIN(lhs_buf->size, rhs_buf->size));
        if (res == 0) {
            if (lhs_buf->size < rhs_buf->size)
                res = -1;
            else if (lhs_buf->size > rhs_buf->size)
                res = 1;
        }

        if (op == UACPI_AML_OP_LLessOp)
            return res < 0;

        return res > 0;
    }

    if (op == UACPI_AML_OP_LLessOp)
        return lhs->integer < rhs->integer;

    return lhs->integer > rhs->integer;
}

static uacpi_status handle_binary_logic(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_aml_op op = op_ctx->op->code;
    uacpi_object *lhs, *rhs, *dst;
    uacpi_bool res;

    lhs = item_array_at(&op_ctx->items, 0)->obj;
    rhs = item_array_at(&op_ctx->items, 1)->obj;
    dst = item_array_at(&op_ctx->items, 2)->obj;

    switch (op) {
    case UACPI_AML_OP_LEqualOp:
    case UACPI_AML_OP_LLessOp:
    case UACPI_AML_OP_LGreaterOp:
        // TODO: typecheck at parse time
        if (lhs->type != rhs->type)
            return UACPI_STATUS_BAD_BYTECODE;

        if (op == UACPI_AML_OP_LEqualOp)
            res = handle_logical_equality(lhs, rhs);
        else
            res = handle_logical_less_or_greater(op, lhs, rhs);
        break;
    default: {
        uacpi_u64 lhs_int, rhs_int;

        // NT only looks at the first 4 bytes of a buffer
        lhs_int = object_to_integer(lhs, 4);
        rhs_int = object_to_integer(rhs, 4);

        if (op == UACPI_AML_OP_LandOp)
            res = lhs_int && rhs_int;
        else
            res = lhs_int || rhs_int;
        break;
    }
    }

    dst->integer = res ? ones() : 0;
    return UACPI_STATUS_OK;
}

/*
 * PkgLength :=
 *   PkgLeadByte |
 *   <pkgleadbyte bytedata> |
 *   <pkgleadbyte bytedata bytedata> | <pkgleadbyte bytedata bytedata bytedata>
 * PkgLeadByte :=
 *   <bit 7-6: bytedata count that follows (0-3)>
 *   <bit 5-4: only used if pkglength < 63>
 *   <bit 3-0: least significant package length nybble>
 */
static uacpi_status parse_package_length(struct call_frame *frame,
                                         struct package_length *out_pkg)
{
    uacpi_u32 left, size;
    uacpi_u8 *data, marker_length;

    out_pkg->begin = frame->code_offset;
    marker_length = 1;

    left = call_frame_code_bytes_left(frame);
    if (uacpi_unlikely(left < 1))
        return UACPI_STATUS_BAD_BYTECODE;

    data = call_frame_cursor(frame);
    marker_length += *data >> 6;

    if (uacpi_unlikely(left < marker_length))
        return UACPI_STATUS_BAD_BYTECODE;

    switch (marker_length) {
    case 1:
        size = *data & 0b111111;
        break;
    case 2:
    case 3:
    case 4: {
        uacpi_u32 temp_byte = 0;

        size = *data & 0b1111;
        uacpi_memcpy(&temp_byte, data + 1, marker_length - 1);

        // marker_length - 1 is at most 3, so this shift is safe
        size |= temp_byte << 4;
        break;
    }
    }

    frame->code_offset += marker_length;
    out_pkg->end = out_pkg->begin + size;
    return UACPI_STATUS_OK;
}

/*
 * ByteData
 * // bit 0-2: ArgCount (0-7)
 * // bit 3: SerializeFlag
 * //   0 NotSerialized
 * //   1 Serialized
 * // bit 4-7: SyncLevel (0x00-0x0f)
 */
static void init_method_flags(uacpi_control_method *method, uacpi_u8 flags_byte)
{
    method->args = flags_byte & 0b111;
    method->is_serialized = (flags_byte >> 3) & 1;
    method->sync_level = flags_byte >> 4;
}

static uacpi_status handle_create_method(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    struct uacpi_control_method *method;
    struct package_length *pkg;
    struct uacpi_namespace_node *node;
    struct uacpi_object *dst;
    uacpi_u32 method_begin_offset;

    method = uacpi_kernel_calloc(1, sizeof(*method));
    if (uacpi_unlikely(method == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    pkg = &item_array_at(&op_ctx->items, 0)->pkg;
    node = item_array_at(&op_ctx->items, 1)->node;
    init_method_flags(method, item_array_at(&op_ctx->items, 2)->immediate);

    method_begin_offset = item_array_at(&op_ctx->items, 3)->immediate;
    method->code = ctx->cur_frame->method->code;
    method->code += method_begin_offset;
    method->size = pkg->end - method_begin_offset;

    dst = item_array_at(&op_ctx->items, 4)->obj;
    dst->method = method;

    node->object = uacpi_create_internal_reference(UACPI_REFERENCE_KIND_NAMED,
                                                   dst);
    if (uacpi_unlikely(node->object == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    return UACPI_STATUS_OK;
}

static uacpi_status handle_create_mutex(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    uacpi_namespace_node *node;
    uacpi_object *dst;

    node = item_array_at(&op_ctx->items, 0)->node;
    dst = item_array_at(&op_ctx->items, 2)->obj;

    // bits 0-3: SyncLevel (0x00-0x0f), bits 4-7: Reserved (must be 0)
    dst->mutex->sync_level = item_array_at(&op_ctx->items, 1)->immediate;
    dst->mutex->sync_level &= 0b1111;

    node->object = uacpi_create_internal_reference(
        UACPI_REFERENCE_KIND_NAMED,
        dst
    );
    if (uacpi_unlikely(node->object == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    return UACPI_STATUS_OK;
}

static uacpi_status handle_create_named(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    struct uacpi_namespace_node *node;
    uacpi_object *src;

    node = item_array_at(&op_ctx->items, 0)->node;
    src = item_array_at(&op_ctx->items, 1)->obj;

    node->object = uacpi_create_internal_reference(UACPI_REFERENCE_KIND_NAMED,
                                                   src);
    if (uacpi_unlikely(node->object == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    return UACPI_STATUS_OK;
}

static uacpi_object_type buffer_field_get_read_type(
    struct uacpi_buffer_field *field
)
{
    if (field->bit_length > (g_uacpi_rt_ctx.is_rev1 ? 32 : 64) ||
        field->force_buffer)
        return UACPI_OBJECT_BUFFER;

    return UACPI_OBJECT_INTEGER;
}

static void do_misaligned_buffer_read(uacpi_buffer_field *field, uacpi_u8 *dst)
{
    struct bit_span src_span = {
        .index = field->bit_index,
        .length = field->bit_length,
        .data = field->backing->data,
    };
    struct bit_span dst_span = {
        .data = dst,
    };

    dst_span.length = buffer_field_byte_size(field) * 8;
    do_rw_misaligned_buffer_field(&dst_span, &src_span);
}

static void do_read_buffer_field(uacpi_buffer_field *field, uacpi_u8 *dst)
{
    if (!(field->bit_index & 7)) {
        uacpi_u8 *src = field->backing->data;
        uacpi_size count;

        count = buffer_field_byte_size(field);
        uacpi_memcpy(dst, src + (field->bit_index / 8), count);

        if (field->bit_length & 7)
            dst[count - 1] &= (1ul << (field->bit_length & 7)) - 1;

        return;
    }

    do_misaligned_buffer_read(field, dst);
}

static uacpi_status handle_field_read(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    struct uacpi_namespace_node *node;
    uacpi_buffer_field *field;
    uacpi_object *dst_obj;
    void *dst;

    node = item_array_at(&op_ctx->items, 0)->node;
    field = &uacpi_namespace_node_get_object(node)->buffer_field;

    dst_obj = item_array_at(&op_ctx->items, 1)->obj;

    if (buffer_field_get_read_type(field) == UACPI_OBJECT_BUFFER) {
        uacpi_buffer *buf;
        uacpi_size buf_size;

        buf = dst_obj->buffer;
        buf_size = buffer_field_byte_size(field);

        dst = uacpi_kernel_calloc(buf_size, 1);
        if (dst == UACPI_NULL)
            return UACPI_STATUS_OUT_OF_MEMORY;

        buf->data = dst;
        buf->size = buf_size;
    } else {
        dst = &dst_obj->integer;
    }

    do_read_buffer_field(field, dst);
    return UACPI_STATUS_OK;
}

static uacpi_status handle_create_buffer_field(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    struct uacpi_namespace_node *node;
    uacpi_buffer *src_buf;
    uacpi_object *field_obj;
    uacpi_buffer_field *field;

    /*
     * Layout of items here:
     * [0] -> Type checked source buffer object
     * [1] -> Byte/bit index integer object
     * [2] (  if     CreateField) -> bit length integer object
     * [3] (2 if not CreateField) -> the new namespace node
     * [4] (3 if not CreateField) -> the buffer field object we're creating here
     */
    src_buf = item_array_at(&op_ctx->items, 0)->obj->buffer;

    if (op_ctx->op->code == UACPI_AML_OP_CreateFieldOp) {
        uacpi_object *idx_obj, *len_obj;

        idx_obj = item_array_at(&op_ctx->items, 1)->obj;
        len_obj = item_array_at(&op_ctx->items, 2)->obj;
        node = item_array_at(&op_ctx->items, 3)->node;
        field_obj = item_array_at(&op_ctx->items, 4)->obj;
        field = &field_obj->buffer_field;

        field->bit_index = idx_obj->integer;

        if (uacpi_unlikely(!len_obj->integer ||
                            len_obj->integer > 0xFFFFFFFF)) {
            uacpi_kernel_log(
                UACPI_LOG_WARN, "invalid bit field length (%llu)\n",
                field->bit_length
            );
            return UACPI_STATUS_BAD_BYTECODE;
        }

        field->bit_length = len_obj->integer;
        field->force_buffer = UACPI_TRUE;
    } else {
        uacpi_object *idx_obj;

        idx_obj = item_array_at(&op_ctx->items, 1)->obj;
        node = item_array_at(&op_ctx->items, 2)->node;
        field_obj = item_array_at(&op_ctx->items, 3)->obj;
        field = &field_obj->buffer_field;

        field->bit_index = idx_obj->integer * 8;
        switch (op_ctx->op->code) {
        case UACPI_AML_OP_CreateBitFieldOp:
            field->bit_length = 1;
            break;
        case UACPI_AML_OP_CreateByteFieldOp:
            field->bit_length = 8;
            break;
        case UACPI_AML_OP_CreateWordFieldOp:
            field->bit_length = 16;
            break;
        case UACPI_AML_OP_CreateDWordFieldOp:
            field->bit_length = 32;
            break;
        case UACPI_AML_OP_CreateQWordFieldOp:
            field->bit_length = 64;
            break;
        default:
            return UACPI_STATUS_INVALID_ARGUMENT;
        }
    }

    if (uacpi_unlikely((field->bit_index + field->bit_length) >
                       src_buf->size * 8)) {
        uacpi_kernel_log(
            UACPI_LOG_WARN,
            "Invalid buffer field: bits [%zu..%zu], buffer size is %zu bytes\n",
            field->bit_length, field->bit_index + field->bit_length,
            src_buf->size
        );
        return UACPI_STATUS_BAD_BYTECODE;
    }

    field->backing = src_buf;
    uacpi_shareable_ref(field->backing);
    node->object = uacpi_create_internal_reference(UACPI_REFERENCE_KIND_NAMED,
                                                   field_obj);
    if (uacpi_unlikely(node->object == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    return UACPI_STATUS_OK;
}

static uacpi_status handle_control_flow(struct execution_context *ctx)
{
    struct call_frame *frame = ctx->cur_frame;
    struct op_context *op_ctx = ctx->cur_op_ctx;

    for (;;) {
        if (ctx->cur_block != frame->last_while) {
            frame_reset_post_end_block(ctx, ctx->cur_block->type);
            continue;
        }

        if (op_ctx->op->code == UACPI_AML_OP_BreakOp)
            frame->code_offset = ctx->cur_block->end;
        else
            frame->code_offset = ctx->cur_block->begin;
        frame_reset_post_end_block(ctx, ctx->cur_block->type);
        break;
    }

    return UACPI_STATUS_OK;
}

static uacpi_status create_named_scope(struct op_context *op_ctx)
{
    uacpi_namespace_node *node;
    uacpi_object *obj;

    node = item_array_at(&op_ctx->items, 1)->node;
    obj = item_array_last(&op_ctx->items)->obj;

    switch (op_ctx->op->code) {
    case UACPI_AML_OP_ProcessorOp: {
        uacpi_processor *proc = &obj->processor;
        proc->id = item_array_at(&op_ctx->items, 2)->immediate;
        proc->block_address = item_array_at(&op_ctx->items, 3)->immediate;
        proc->block_length = item_array_at(&op_ctx->items, 4)->immediate;
        break;
    }

    case UACPI_AML_OP_PowerResOp: {
        uacpi_power_resource *power_res = &obj->power_resource;
        power_res->system_level = item_array_at(&op_ctx->items, 2)->immediate;
        power_res->resource_order = item_array_at(&op_ctx->items, 3)->immediate;
        break;
    }

    default:
        break;
    }

    node->object = uacpi_create_internal_reference(UACPI_REFERENCE_KIND_NAMED,
                                                   obj);
    if (uacpi_unlikely(node->object == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    return UACPI_STATUS_OK;
}

static uacpi_status handle_code_block(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;
    struct package_length *pkg;
    uacpi_bool skip_block;

    pkg = &item_array_at(&op_ctx->items, 0)->pkg;

    switch (op_ctx->op->code) {
    case UACPI_AML_OP_ElseOp:
        skip_block = ctx->skip_else;
        break;
    case UACPI_AML_OP_ProcessorOp:
    case UACPI_AML_OP_PowerResOp:
    case UACPI_AML_OP_ThermalZoneOp:
    case UACPI_AML_OP_DeviceOp: {
        uacpi_status ret;

        ret = create_named_scope(op_ctx);
        if (uacpi_unlikely_error(ret))
            return ret;

        // FALLTHROUGH intended
    }
    case UACPI_AML_OP_ScopeOp:
        skip_block = UACPI_FALSE;
        break;
    case UACPI_AML_OP_IfOp:
    case UACPI_AML_OP_WhileOp: {
        uacpi_object *operand;

        operand = item_array_at(&op_ctx->items, 1)->obj;
        skip_block = operand->integer == 0;
        break;
    }
    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    if (skip_block) {
        ctx->cur_frame->code_offset = pkg->end;
        return UACPI_STATUS_OK;
    }

    return begin_block_execution(ctx);
}

static uacpi_status handle_return(struct execution_context *ctx)
{
    uacpi_status ret;
    uacpi_object *dst = UACPI_NULL;

    ctx->cur_frame->code_offset = ctx->cur_frame->method->size;
    ret = method_get_ret_object(ctx, &dst);

    if (uacpi_unlikely_error(ret))
        return ret;
    if (dst == UACPI_NULL)
        return UACPI_STATUS_OK;

    /*
     * Should be possible to move here if method returns a literal
     * like Return(Buffer { ... }), otherwise we have to copy just to
     * be safe.
     */
    return uacpi_object_assign(
        dst,
        item_array_at(&ctx->cur_op_ctx->items, 0)->obj,
        UACPI_ASSIGN_BEHAVIOR_DEEP_COPY
    );
}

static void refresh_ctx_pointers(struct execution_context *ctx)
{
    struct call_frame *frame = ctx->cur_frame;

    if (frame == UACPI_NULL) {
        ctx->cur_op_ctx = UACPI_NULL;
        ctx->prev_op_ctx = UACPI_NULL;
        ctx->cur_block = UACPI_NULL;
        return;
    }

    ctx->cur_op_ctx = op_context_array_last(&frame->pending_ops);
    ctx->prev_op_ctx = op_context_array_one_before_last(&frame->pending_ops);
    ctx->cur_block = code_block_array_last(&frame->code_blocks);
}

static bool ctx_has_non_preempted_op(struct execution_context *ctx)
{
    return ctx->cur_op_ctx && !ctx->cur_op_ctx->preempted;
}

#define UACPI_OP_TRACING

static void trace_op(const struct uacpi_op_spec *op)
{
#ifdef UACPI_OP_TRACING
    uacpi_kernel_log(UACPI_LOG_TRACE, "Processing Op '%s' (0x%04X)\n",
                     op->name, op->code);
#endif
}

static uacpi_status frame_push_args(struct call_frame *frame,
                                    struct op_context *op_ctx)
{
    uacpi_size i;

    /*
     * MethodCall items:
     * items[0] -> method namespace node
     * items[1] -> immediate that was used for parsing the arguments
     * items[2...nargs-1] -> method arguments
     * items[-1] -> return value object
     *
     * Here we only care about the arguments though.
     */
    for (i = 2; i < item_array_size(&op_ctx->items) - 1; i++) {
        uacpi_object *src, *dst;

        src = item_array_at(&op_ctx->items, i)->obj;

        dst = uacpi_create_internal_reference(UACPI_REFERENCE_KIND_ARG, src);
        if (uacpi_unlikely(dst == UACPI_NULL))
            return UACPI_STATUS_OUT_OF_MEMORY;

        frame->args[i - 2] = dst;
    }

    return UACPI_STATUS_OK;
}

static uacpi_status frame_setup_base_scope(struct call_frame *frame,
                                           uacpi_namespace_node *scope,
                                           uacpi_control_method *method)
{
    struct code_block *block;

    block = code_block_array_alloc(&frame->code_blocks);
    if (uacpi_unlikely(block == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    block->type = CODE_BLOCK_SCOPE;
    block->node = scope;
    block->begin = 0;
    block->end = method->size;
    frame->method = method;
    frame->cur_scope = scope;
    return UACPI_STATUS_OK;
}

static uacpi_status push_new_frame(struct execution_context *ctx,
                                   struct call_frame **out_frame)
{
    struct call_frame_array *call_stack = &ctx->call_stack;
    struct call_frame *prev_frame;

    *out_frame = call_frame_array_calloc(call_stack);
    if (uacpi_unlikely(*out_frame == UACPI_NULL))
        return UACPI_STATUS_OUT_OF_MEMORY;

    /*
     * Allocating a new frame might have reallocated the dynamic buffer so our
     * execution_context members might now be pointing to freed memory.
     * Refresh them here.
     */
    prev_frame = call_frame_array_at(call_stack,
                                     call_frame_array_size(call_stack) - 2);
    ctx->cur_frame = prev_frame;
    refresh_ctx_pointers(ctx);

    return UACPI_STATUS_OK;
}

static uacpi_bool maybe_end_block(struct execution_context *ctx)
{
    struct code_block *block = ctx->cur_block;
    struct call_frame *cur_frame = ctx->cur_frame;

    if (!block)
        return UACPI_FALSE;
    if (cur_frame->code_offset != block->end)
        return UACPI_FALSE;

    ctx->skip_else = UACPI_FALSE;

    if (block->type == CODE_BLOCK_WHILE) {
        cur_frame->code_offset = block->begin;
    } else if (block->type == CODE_BLOCK_IF) {
        ctx->skip_else = UACPI_TRUE;
    }

    frame_reset_post_end_block(ctx, block->type);
    return UACPI_TRUE;
}

static uacpi_status store_to_target(uacpi_object *dst, uacpi_object *src)
{
    uacpi_status ret;

    switch (dst->type) {
    case UACPI_OBJECT_DEBUG:
        ret = debug_store(src);
        break;
    case UACPI_OBJECT_REFERENCE:
        ret = store_to_reference(dst, src);
        break;

    case UACPI_OBJECT_BUFFER_INDEX:
        ret = object_assign_with_implicit_cast(dst, src);
        break;

    case UACPI_OBJECT_INTEGER:
        // NULL target
        if (dst->integer == 0) {
            ret = UACPI_STATUS_OK;
            break;
        }
    default:
        ret = UACPI_STATUS_BAD_BYTECODE;
    }

    return ret;
}

static uacpi_status handle_copy_object_or_store(struct execution_context *ctx)
{
    uacpi_object *src, *dst;
    struct op_context *op_ctx = ctx->cur_op_ctx;

    src = item_array_at(&op_ctx->items, 0)->obj;
    dst = item_array_at(&op_ctx->items, 1)->obj;

    if (op_ctx->op->code == UACPI_AML_OP_StoreOp)
        return store_to_target(dst, src);

    if (dst->type != UACPI_OBJECT_REFERENCE)
        return UACPI_STATUS_BAD_BYTECODE;

    return copy_object_to_reference(dst, src);
}

static uacpi_status push_op(struct execution_context *ctx)
{
    struct call_frame *frame = ctx->cur_frame;
    struct op_context *op_ctx;

    op_ctx = op_context_array_calloc(&frame->pending_ops);
    if (op_ctx == UACPI_NULL)
        return UACPI_STATUS_OUT_OF_MEMORY;

    op_ctx->op = ctx->cur_op;
    refresh_ctx_pointers(ctx);
    return UACPI_STATUS_OK;
}

static void pop_op(struct execution_context *ctx)
{
    struct call_frame *frame = ctx->cur_frame;
    struct op_context *cur_op_ctx = ctx->cur_op_ctx;

    for (;;) {
        struct item *item;

        item = item_array_last(&cur_op_ctx->items);

        if (item == UACPI_NULL)
            break;
        if (item->type == ITEM_OBJECT)
            uacpi_object_unref(item->obj);
        if (item->type == ITEM_NAMESPACE_NODE_METHOD_LOCAL)
            uacpi_namespace_node_free(item->node);

        item_array_pop(&cur_op_ctx->items);
    }

    item_array_clear(&cur_op_ctx->items);
    op_context_array_pop(&frame->pending_ops);
    refresh_ctx_pointers(ctx);
}

static uacpi_u8 parse_op_generates_item[0x100] = {
    [UACPI_PARSE_OP_SIMPLE_NAME] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_SUPERNAME] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_SUPERNAME_IMPLICIT_DEREF] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_SUPERNAME_OR_UNRESOLVED] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_TERM_ARG] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_TERM_ARG_UNWRAP_INTERNAL] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT_OR_UNRESOLVED] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_OPERAND] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_COMPUTATIONAL_DATA] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_TARGET] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_PKGLEN] = ITEM_PACKAGE_LENGTH,
    [UACPI_PARSE_OP_TRACKED_PKGLEN] = ITEM_PACKAGE_LENGTH,
    [UACPI_PARSE_OP_CREATE_NAMESTRING] = ITEM_NAMESPACE_NODE_METHOD_LOCAL,
    [UACPI_PARSE_OP_EXISTING_NAMESTRING] = ITEM_NAMESPACE_NODE,
    [UACPI_PARSE_OP_EXISTING_NAMESTRING_OR_NULL] = ITEM_NAMESPACE_NODE,
    [UACPI_PARSE_OP_LOAD_INLINE_IMM_AS_OBJECT] = ITEM_OBJECT,
    [UACPI_PARSE_OP_LOAD_INLINE_IMM] = ITEM_IMMEDIATE,
    [UACPI_PARSE_OP_LOAD_IMM] = ITEM_IMMEDIATE,
    [UACPI_PARSE_OP_LOAD_IMM_AS_OBJECT] = ITEM_OBJECT,
    [UACPI_PARSE_OP_LOAD_FALSE_OBJECT] = ITEM_OBJECT,
    [UACPI_PARSE_OP_LOAD_TRUE_OBJECT] = ITEM_OBJECT,
    [UACPI_PARSE_OP_OBJECT_ALLOC] = ITEM_OBJECT,
    [UACPI_PARSE_OP_OBJECT_ALLOC_TYPED] = ITEM_OBJECT,
    [UACPI_PARSE_OP_EMPTY_OBJECT_ALLOC] = ITEM_EMPTY_OBJECT,
    [UACPI_PARSE_OP_OBJECT_CONVERT_TO_SHALLOW_COPY] = ITEM_OBJECT,
    [UACPI_PARSE_OP_OBJECT_CONVERT_TO_DEEP_COPY] = ITEM_OBJECT,
    [UACPI_PARSE_OP_RECORD_AML_PC] = ITEM_IMMEDIATE,
};

static const uacpi_u8 *op_decode_cursor(const struct op_context *ctx)
{
    const struct uacpi_op_spec *spec = ctx->op;

    if (spec->properties & UACPI_OP_PROPERTY_OUT_OF_LINE)
        return &spec->indirect_decode_ops[ctx->pc];

    return &spec->decode_ops[ctx->pc];
}

static uacpi_u8 op_decode_byte(struct op_context *ctx)
{
    uacpi_u8 byte;

    byte = *op_decode_cursor(ctx);
    ctx->pc++;

    return byte;
}

// MSVC doesn't support __VA_OPT__ so we do this weirdness
#define EXEC_OP_DO_WARN(reason, ...)                                 \
    uacpi_kernel_log(UACPI_LOG_WARN, "Op 0x%04X ('%s'): "reason"\n", \
                     op_ctx->op->code, op_ctx->op->name __VA_ARGS__)

#define EXEC_OP_WARN_2(reason, arg0, arg1) EXEC_OP_DO_WARN(reason, ,arg0, arg1)
#define EXEC_OP_WARN_1(reason, arg0) EXEC_OP_DO_WARN(reason, ,arg0)
#define EXEC_OP_WARN(reason) EXEC_OP_DO_WARN(reason)

#define SPEC_SIMPLE_NAME "SimpleName := NameString | ArgObj | LocalObj"
#define SPEC_SUPER_NAME \
    "SuperName := SimpleName | DebugObj | ReferenceTypeOpcode"
#define SPEC_TERM_ARG \
    "TermArg := ExpressionOpcode | DataObject | ArgObj | LocalObj"
#define SPEC_OPERAND "Operand := TermArg => Integer"
#define SPEC_TARGET "Target := SuperName | NullName"

#define SPEC_COMPUTATIONAL_DATA                                             \
    "ComputationalData := ByteConst | WordConst | DWordConst | QWordConst " \
    "| String | ConstObj | RevisionOp | DefBuffer"

static uacpi_bool op_wants_supername(enum uacpi_parse_op op)
{
    switch (op) {
    case UACPI_PARSE_OP_SIMPLE_NAME:
    case UACPI_PARSE_OP_SUPERNAME:
    case UACPI_PARSE_OP_SUPERNAME_IMPLICIT_DEREF:
    case UACPI_PARSE_OP_SUPERNAME_OR_UNRESOLVED:
    case UACPI_PARSE_OP_TARGET:
        return UACPI_TRUE;
    default:
        return UACPI_FALSE;
    }
}

static uacpi_bool op_wants_term_arg_or_operand(enum uacpi_parse_op op)
{
    switch (op) {
    case UACPI_PARSE_OP_TERM_ARG:
    case UACPI_PARSE_OP_TERM_ARG_UNWRAP_INTERNAL:
    case UACPI_PARSE_OP_OPERAND:
    case UACPI_PARSE_OP_COMPUTATIONAL_DATA:
        return UACPI_TRUE;
    default:
        return UACPI_FALSE;
    }
}

static uacpi_bool op_allows_unresolved(enum uacpi_parse_op op)
{
    switch (op) {
    case UACPI_PARSE_OP_SUPERNAME_OR_UNRESOLVED:
    case UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT_OR_UNRESOLVED:
    case UACPI_PARSE_OP_EXISTING_NAMESTRING_OR_NULL:
        return UACPI_TRUE;
    default:
        return UACPI_FALSE;
    }
}

static uacpi_status op_typecheck(const struct op_context *op_ctx,
                                 const struct op_context *cur_op_ctx)
{
    const char *expected_type_str;
    uacpi_u8 ok_mask = 0;
    uacpi_u8 props = cur_op_ctx->op->properties;

    switch (*op_decode_cursor(op_ctx)) {
    // SimpleName := NameString | ArgObj | LocalObj
    case UACPI_PARSE_OP_SIMPLE_NAME:
        expected_type_str = SPEC_SIMPLE_NAME;
        ok_mask |= UACPI_OP_PROPERTY_SIMPLE_NAME;
        break;

    // Target := SuperName | NullName
    case UACPI_PARSE_OP_TARGET:
        expected_type_str = SPEC_TARGET;
        ok_mask |= UACPI_OP_PROPERTY_TARGET | UACPI_OP_PROPERTY_SUPERNAME;
        break;

    // SuperName := SimpleName | DebugObj | ReferenceTypeOpcode
    case UACPI_PARSE_OP_SUPERNAME:
    case UACPI_PARSE_OP_SUPERNAME_IMPLICIT_DEREF:
    case UACPI_PARSE_OP_SUPERNAME_OR_UNRESOLVED:
        expected_type_str = SPEC_SUPER_NAME;
        ok_mask |= UACPI_OP_PROPERTY_SUPERNAME;
        break;

    // TermArg := ExpressionOpcode | DataObject | ArgObj | LocalObj
    case UACPI_PARSE_OP_TERM_ARG:
    case UACPI_PARSE_OP_TERM_ARG_UNWRAP_INTERNAL:
    case UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT:
    case UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT_OR_UNRESOLVED:
    case UACPI_PARSE_OP_OPERAND:
    case UACPI_PARSE_OP_COMPUTATIONAL_DATA:
        expected_type_str = SPEC_TERM_ARG;
        ok_mask |= UACPI_OP_PROPERTY_TERM_ARG;
        break;
    }

    if (!(props & ok_mask)) {
        EXEC_OP_WARN_2("invalid argument: '%s', expected a %s",
                       cur_op_ctx->op->name, expected_type_str);
        return UACPI_STATUS_BAD_BYTECODE;
    }

    return UACPI_STATUS_OK;
}

static uacpi_status typecheck_operand(
    const struct op_context *op_ctx,
    const uacpi_object *obj
)
{
    if (uacpi_likely(obj->type == UACPI_OBJECT_INTEGER))
        return UACPI_STATUS_OK;

    EXEC_OP_WARN_2("invalid argument type: %s, expected a %s",
                   uacpi_object_type_to_string(obj->type), SPEC_OPERAND);
    return UACPI_STATUS_BAD_BYTECODE;
}

static uacpi_status typecheck_computational_data(
    const struct op_context *op_ctx,
    const uacpi_object *obj
)
{
    switch (obj->type) {
    case UACPI_OBJECT_STRING:
    case UACPI_OBJECT_BUFFER:
    case UACPI_OBJECT_INTEGER:
        return UACPI_STATUS_OK;
    default:
        EXEC_OP_WARN_2(
            "invalid argument type: %s, expected a %s",
            uacpi_object_type_to_string(obj->type),
            SPEC_COMPUTATIONAL_DATA
        );
        return UACPI_STATUS_BAD_BYTECODE;
    }
}

static uacpi_status uninstalled_op_handler(struct execution_context *ctx)
{
    struct op_context *op_ctx = ctx->cur_op_ctx;

    EXEC_OP_WARN("no dedicated handler installed");
    return UACPI_STATUS_UNIMPLEMENTED;
}

enum op_handler {
    OP_HANDLER_UNINSTALLED = 0,
    OP_HANDLER_LOCAL,
    OP_HANDLER_ARG,
    OP_HANDLER_STRING,
    OP_HANDLER_BINARY_MATH,
    OP_HANDLER_CONTROL_FLOW,
    OP_HANDLER_CODE_BLOCK,
    OP_HANDLER_RETURN,
    OP_HANDLER_CREATE_METHOD,
    OP_HANDLER_COPY_OBJECT_OR_STORE,
    OP_HANDLER_INC_DEC,
    OP_HANDLER_REF_OR_DEREF_OF,
    OP_HANDLER_LOGICAL_NOT,
    OP_HANDLER_BINARY_LOGIC,
    OP_HANDLER_NAMED_OBJECT,
    OP_HANDLER_BUFFER,
    OP_HANDLER_PACKAGE,
    OP_HANDLER_CREATE_NAMED,
    OP_HANDLER_CREATE_BUFFER_FIELD,
    OP_HANDLER_READ_FIELD,
    OP_HANDLER_ALIAS,
    OP_HANDLER_CONCATENATE,
    OP_HANDLER_SIZEOF,
    OP_HANDLER_UNARY_MATH,
    OP_HANDLER_INDEX,
    OP_HANDLER_OBJECT_TYPE,
    OP_HANDLER_CREATE_OP_REGION,
    OP_HANDLER_CREATE_FIELD,
    OP_HANDLER_TO,
    OP_HANDLER_TO_STRING,
    OP_HANDLER_TIMER,
    OP_HANDLER_MID,
    OP_HANDLER_CREATE_MUTEX,
};

static uacpi_status (*op_handlers[])(struct execution_context *ctx) = {
    /*
     * All OPs that don't have a handler dispatch to here if
     * UACPI_PARSE_OP_INVOKE_HANDLER is reached.
     */
    [OP_HANDLER_UNINSTALLED] = uninstalled_op_handler,
    [OP_HANDLER_LOCAL] = handle_local,
    [OP_HANDLER_ARG] = handle_arg,
    [OP_HANDLER_NAMED_OBJECT] = handle_named_object,
    [OP_HANDLER_STRING] = handle_string,
    [OP_HANDLER_BINARY_MATH] = handle_binary_math,
    [OP_HANDLER_CONTROL_FLOW] = handle_control_flow,
    [OP_HANDLER_CODE_BLOCK] = handle_code_block,
    [OP_HANDLER_RETURN] = handle_return,
    [OP_HANDLER_CREATE_METHOD] = handle_create_method,
    [OP_HANDLER_CREATE_MUTEX] = handle_create_mutex,
    [OP_HANDLER_COPY_OBJECT_OR_STORE] = handle_copy_object_or_store,
    [OP_HANDLER_INC_DEC] = handle_inc_dec,
    [OP_HANDLER_REF_OR_DEREF_OF] = handle_ref_or_deref_of,
    [OP_HANDLER_LOGICAL_NOT] = handle_logical_not,
    [OP_HANDLER_BINARY_LOGIC] = handle_binary_logic,
    [OP_HANDLER_BUFFER] = handle_buffer,
    [OP_HANDLER_PACKAGE] = handle_package,
    [OP_HANDLER_CREATE_NAMED] = handle_create_named,
    [OP_HANDLER_CREATE_BUFFER_FIELD] = handle_create_buffer_field,
    [OP_HANDLER_READ_FIELD] = handle_field_read,
    [OP_HANDLER_TO] = handle_to,
    [OP_HANDLER_ALIAS] = handle_create_alias,
    [OP_HANDLER_CONCATENATE] = handle_concatenate,
    [OP_HANDLER_SIZEOF] = handle_sizeof,
    [OP_HANDLER_UNARY_MATH] = handle_unary_math,
    [OP_HANDLER_INDEX] = handle_index,
    [OP_HANDLER_OBJECT_TYPE] = handle_object_type,
    [OP_HANDLER_CREATE_OP_REGION] = handle_create_op_region,
    [OP_HANDLER_CREATE_FIELD] = handle_create_field,
    [OP_HANDLER_TIMER] = handle_timer,
    [OP_HANDLER_TO_STRING] = handle_to_string,
    [OP_HANDLER_MID] = handle_mid,
};

static uacpi_u8 handler_idx_of_op[0x100] = {
    [UACPI_AML_OP_Local0Op] = OP_HANDLER_LOCAL,
    [UACPI_AML_OP_Local1Op] = OP_HANDLER_LOCAL,
    [UACPI_AML_OP_Local2Op] = OP_HANDLER_LOCAL,
    [UACPI_AML_OP_Local3Op] = OP_HANDLER_LOCAL,
    [UACPI_AML_OP_Local4Op] = OP_HANDLER_LOCAL,
    [UACPI_AML_OP_Local5Op] = OP_HANDLER_LOCAL,
    [UACPI_AML_OP_Local6Op] = OP_HANDLER_LOCAL,
    [UACPI_AML_OP_Local7Op] = OP_HANDLER_LOCAL,

    [UACPI_AML_OP_Arg0Op] = OP_HANDLER_ARG,
    [UACPI_AML_OP_Arg1Op] = OP_HANDLER_ARG,
    [UACPI_AML_OP_Arg2Op] = OP_HANDLER_ARG,
    [UACPI_AML_OP_Arg3Op] = OP_HANDLER_ARG,
    [UACPI_AML_OP_Arg4Op] = OP_HANDLER_ARG,
    [UACPI_AML_OP_Arg5Op] = OP_HANDLER_ARG,
    [UACPI_AML_OP_Arg6Op] = OP_HANDLER_ARG,

    [UACPI_AML_OP_StringPrefix] = OP_HANDLER_STRING,

    [UACPI_AML_OP_AddOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_SubtractOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_MultiplyOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_DivideOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_ShiftLeftOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_ShiftRightOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_AndOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_NandOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_OrOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_NorOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_XorOp] = OP_HANDLER_BINARY_MATH,
    [UACPI_AML_OP_ModOp] = OP_HANDLER_BINARY_MATH,

    [UACPI_AML_OP_IfOp] = OP_HANDLER_CODE_BLOCK,
    [UACPI_AML_OP_ElseOp] = OP_HANDLER_CODE_BLOCK,
    [UACPI_AML_OP_WhileOp] = OP_HANDLER_CODE_BLOCK,
    [UACPI_AML_OP_ScopeOp] = OP_HANDLER_CODE_BLOCK,

    [UACPI_AML_OP_ContinueOp] = OP_HANDLER_CONTROL_FLOW,
    [UACPI_AML_OP_BreakOp] = OP_HANDLER_CONTROL_FLOW,

    [UACPI_AML_OP_ReturnOp] = OP_HANDLER_RETURN,

    [UACPI_AML_OP_MethodOp] = OP_HANDLER_CREATE_METHOD,

    [UACPI_AML_OP_StoreOp] = OP_HANDLER_COPY_OBJECT_OR_STORE,
    [UACPI_AML_OP_CopyObjectOp] = OP_HANDLER_COPY_OBJECT_OR_STORE,

    [UACPI_AML_OP_IncrementOp] = OP_HANDLER_INC_DEC,
    [UACPI_AML_OP_DecrementOp] = OP_HANDLER_INC_DEC,

    [UACPI_AML_OP_RefOfOp] = OP_HANDLER_REF_OR_DEREF_OF,
    [UACPI_AML_OP_DerefOfOp] = OP_HANDLER_REF_OR_DEREF_OF,

    [UACPI_AML_OP_LnotOp] = OP_HANDLER_LOGICAL_NOT,

    [UACPI_AML_OP_LEqualOp] = OP_HANDLER_BINARY_LOGIC,
    [UACPI_AML_OP_LandOp] = OP_HANDLER_BINARY_LOGIC,
    [UACPI_AML_OP_LorOp] = OP_HANDLER_BINARY_LOGIC,
    [UACPI_AML_OP_LGreaterOp] = OP_HANDLER_BINARY_LOGIC,
    [UACPI_AML_OP_LLessOp] = OP_HANDLER_BINARY_LOGIC,

    [UACPI_AML_OP_InternalOpNamedObject] = OP_HANDLER_NAMED_OBJECT,

    [UACPI_AML_OP_BufferOp] = OP_HANDLER_BUFFER,

    [UACPI_AML_OP_PackageOp] = OP_HANDLER_PACKAGE,
    [UACPI_AML_OP_VarPackageOp] = OP_HANDLER_PACKAGE,

    [UACPI_AML_OP_NameOp] = OP_HANDLER_CREATE_NAMED,

    [UACPI_AML_OP_CreateBitFieldOp] = OP_HANDLER_CREATE_BUFFER_FIELD,
    [UACPI_AML_OP_CreateByteFieldOp] = OP_HANDLER_CREATE_BUFFER_FIELD,
    [UACPI_AML_OP_CreateWordFieldOp] = OP_HANDLER_CREATE_BUFFER_FIELD,
    [UACPI_AML_OP_CreateDWordFieldOp] = OP_HANDLER_CREATE_BUFFER_FIELD,
    [UACPI_AML_OP_CreateQWordFieldOp] = OP_HANDLER_CREATE_BUFFER_FIELD,

    [UACPI_AML_OP_InternalOpReadFieldAsBuffer] = OP_HANDLER_READ_FIELD,
    [UACPI_AML_OP_InternalOpReadFieldAsInteger] = OP_HANDLER_READ_FIELD,

    [UACPI_AML_OP_ToIntegerOp] = OP_HANDLER_TO,
    [UACPI_AML_OP_ToBufferOp] = OP_HANDLER_TO,
    [UACPI_AML_OP_ToDecimalStringOp] = OP_HANDLER_TO,
    [UACPI_AML_OP_ToHexStringOp] = OP_HANDLER_TO,
    [UACPI_AML_OP_ToStringOp] = OP_HANDLER_TO_STRING,

    [UACPI_AML_OP_AliasOp] = OP_HANDLER_ALIAS,

    [UACPI_AML_OP_ConcatOp] = OP_HANDLER_CONCATENATE,

    [UACPI_AML_OP_SizeOfOp] = OP_HANDLER_SIZEOF,

    [UACPI_AML_OP_NotOp] = OP_HANDLER_UNARY_MATH,
    [UACPI_AML_OP_FindSetLeftBitOp] = OP_HANDLER_UNARY_MATH,
    [UACPI_AML_OP_FindSetRightBitOp] = OP_HANDLER_UNARY_MATH,

    [UACPI_AML_OP_IndexOp] = OP_HANDLER_INDEX,

    [UACPI_AML_OP_ObjectTypeOp] = OP_HANDLER_OBJECT_TYPE,

    [UACPI_AML_OP_MidOp] = OP_HANDLER_MID,
};

#define EXT_OP_IDX(op) (op & 0xFF)

static uacpi_u8 handler_idx_of_ext_op[0x100] = {
    [EXT_OP_IDX(UACPI_AML_OP_CreateFieldOp)] = OP_HANDLER_CREATE_BUFFER_FIELD,
    [EXT_OP_IDX(UACPI_AML_OP_CondRefOfOp)] = OP_HANDLER_REF_OR_DEREF_OF,
    [EXT_OP_IDX(UACPI_AML_OP_OpRegionOp)] = OP_HANDLER_CREATE_OP_REGION,
    [EXT_OP_IDX(UACPI_AML_OP_FieldOp)] = OP_HANDLER_CREATE_FIELD,
    [EXT_OP_IDX(UACPI_AML_OP_DeviceOp)] = OP_HANDLER_CODE_BLOCK,
    [EXT_OP_IDX(UACPI_AML_OP_ProcessorOp)] = OP_HANDLER_CODE_BLOCK,
    [EXT_OP_IDX(UACPI_AML_OP_PowerResOp)] = OP_HANDLER_CODE_BLOCK,
    [EXT_OP_IDX(UACPI_AML_OP_ThermalZoneOp)] = OP_HANDLER_CODE_BLOCK,
    [EXT_OP_IDX(UACPI_AML_OP_TimerOp)] = OP_HANDLER_TIMER,
    [EXT_OP_IDX(UACPI_AML_OP_MutexOp)] = OP_HANDLER_CREATE_MUTEX,
};

static uacpi_status exec_op(struct execution_context *ctx)
{
    uacpi_status ret = UACPI_STATUS_OK;
    struct call_frame *frame = ctx->cur_frame;
    struct op_context *op_ctx;
    struct item *item = UACPI_NULL;
    enum uacpi_parse_op prev_op = 0, op;

    /*
     * Allocate a new op context if previous is preempted (looking for a
     * dynamic argument), or doesn't exist at all.
     */
    if (!ctx_has_non_preempted_op(ctx)) {
        ret = push_op(ctx);
        if (uacpi_unlikely_error(ret))
            return ret;
    }

    if (ctx->prev_op_ctx)
        prev_op = *op_decode_cursor(ctx->prev_op_ctx);

    for (;;) {
        if (uacpi_unlikely_error(ret))
            return ret;

        op_ctx = ctx->cur_op_ctx;
        frame = ctx->cur_frame;

        if (op_ctx->pc == 0 && ctx->prev_op_ctx) {
            /*
             * Type check the current arg type against what is expected by the
             * preempted op. This check is able to catch most type violations
             * with the only exception being Operand as we only know whether
             * that evaluates to an integer after the fact.
             */
            ret = op_typecheck(ctx->prev_op_ctx, ctx->cur_op_ctx);
            if (uacpi_unlikely_error(ret))
                return ret;
        }

        op = op_decode_byte(op_ctx);

        if (parse_op_generates_item[op] != ITEM_NONE) {
            item = item_array_alloc(&op_ctx->items);
            if (uacpi_unlikely(item == UACPI_NULL))
                return UACPI_STATUS_OUT_OF_MEMORY;

            item->type = parse_op_generates_item[op];
            if (item->type == ITEM_OBJECT) {
                enum uacpi_object_type type = UACPI_OBJECT_UNINITIALIZED;

                if (op == UACPI_PARSE_OP_OBJECT_ALLOC_TYPED)
                    type = op_decode_byte(op_ctx);

                item->obj = uacpi_create_object(type);
                if (uacpi_unlikely(item->obj == UACPI_NULL))
                    return UACPI_STATUS_OUT_OF_MEMORY;
            } else if (item->type == ITEM_EMPTY_OBJECT) {
                item->obj = UACPI_NULL;
            }
        } else if (item == UACPI_NULL) {
            item = item_array_last(&op_ctx->items);
        }

        switch (op) {
        case UACPI_PARSE_OP_END: {
            if (op_ctx->tracked_pkg_idx) {
                item = item_array_at(&op_ctx->items, op_ctx->tracked_pkg_idx - 1);
                frame->code_offset = item->pkg.end;
            }

            pop_op(ctx);
            if (ctx->cur_op_ctx) {
                ctx->cur_op_ctx->preempted = UACPI_FALSE;
                ctx->cur_op_ctx->pc++;
            }

            return UACPI_STATUS_OK;
        }

        case UACPI_PARSE_OP_SIMPLE_NAME:
        case UACPI_PARSE_OP_SUPERNAME:
        case UACPI_PARSE_OP_SUPERNAME_IMPLICIT_DEREF:
        case UACPI_PARSE_OP_SUPERNAME_OR_UNRESOLVED:
        case UACPI_PARSE_OP_TERM_ARG:
        case UACPI_PARSE_OP_TERM_ARG_UNWRAP_INTERNAL:
        case UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT:
        case UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT_OR_UNRESOLVED:
        case UACPI_PARSE_OP_OPERAND:
        case UACPI_PARSE_OP_COMPUTATIONAL_DATA:
        case UACPI_PARSE_OP_TARGET:
            /*
             * Preempt this op parsing for now as we wait for the dynamic arg
             * to be parsed.
             */
            op_ctx->preempted = UACPI_TRUE;
            op_ctx->pc--;
            return UACPI_STATUS_OK;

        case UACPI_PARSE_OP_TRACKED_PKGLEN:
            op_ctx->tracked_pkg_idx = item_array_size(&op_ctx->items);
        case UACPI_PARSE_OP_PKGLEN:
            ret = parse_package_length(frame, &item->pkg);
            break;

        case UACPI_PARSE_OP_LOAD_INLINE_IMM:
        case UACPI_PARSE_OP_LOAD_INLINE_IMM_AS_OBJECT: {
            void *dst;
            uacpi_u8 src_width;

            if (op == UACPI_PARSE_OP_LOAD_INLINE_IMM_AS_OBJECT) {
                item->obj->type = UACPI_OBJECT_INTEGER;
                dst = &item->obj->integer;
                src_width = 8;
            } else {
                dst = &item->immediate;
                src_width = op_decode_byte(op_ctx);
            }

            uacpi_memcpy_zerout(
                dst, op_decode_cursor(op_ctx),
                sizeof(uacpi_u64), src_width
            );
            op_ctx->pc += src_width;
            break;
        }

        case UACPI_PARSE_OP_LOAD_IMM:
        case UACPI_PARSE_OP_LOAD_IMM_AS_OBJECT: {
            uacpi_u8 width;
            void *dst;

            width = op_decode_byte(op_ctx);
            if (uacpi_unlikely(call_frame_code_bytes_left(frame) < width))
                return UACPI_STATUS_BAD_BYTECODE;

            if (op == UACPI_PARSE_OP_LOAD_IMM_AS_OBJECT) {
                item->obj->type = UACPI_OBJECT_INTEGER;
                item->obj->integer = 0;
                dst = &item->obj->integer;
            } else {
                item->immediate = 0;
                dst = item->immediate_bytes;
            }

            uacpi_memcpy(dst, call_frame_cursor(frame), width);
            frame->code_offset += width;
            break;
        }

        case UACPI_PARSE_OP_LOAD_FALSE_OBJECT:
        case UACPI_PARSE_OP_LOAD_TRUE_OBJECT: {
            uacpi_object *obj = item->obj;
            obj->type = UACPI_OBJECT_INTEGER;
            obj->integer = op == UACPI_PARSE_OP_LOAD_FALSE_OBJECT ? 0 : ones();
            break;
        }

        case UACPI_PARSE_OP_RECORD_AML_PC:
            item->immediate = frame->code_offset;
            break;

        case UACPI_PARSE_OP_TRUNCATE_NUMBER:
            truncate_number_if_needed(item->obj);
            break;

        case UACPI_PARSE_OP_TYPECHECK: {
            enum uacpi_object_type expected_type;

            expected_type = op_decode_byte(op_ctx);

            if (uacpi_unlikely(item->obj->type != expected_type)) {
                EXEC_OP_WARN_2("bad object type: expected %d, got %d!",
                               expected_type, item->obj->type);
                ret = UACPI_STATUS_BAD_BYTECODE;
            }

            break;
        }

        case UACPI_PARSE_OP_TODO:
            EXEC_OP_WARN("not yet implemented");
            ret = UACPI_STATUS_UNIMPLEMENTED;
            break;

        case UACPI_PARSE_OP_BAD_OPCODE:
        case UACPI_PARSE_OP_UNREACHABLE:
            EXEC_OP_WARN("invalid/unexpected opcode");
            ret = UACPI_STATUS_BAD_BYTECODE;
            break;

        case UACPI_PARSE_OP_AML_PC_DECREMENT:
            frame->code_offset--;
            break;

        case UACPI_PARSE_OP_IMM_DECREMENT:
            item_array_at(&op_ctx->items, op_decode_byte(op_ctx))->immediate--;
            break;

        case UACPI_PARSE_OP_IF_HAS_DATA: {
            uacpi_size pkg_idx = op_ctx->tracked_pkg_idx - 1;
            struct package_length *pkg;
            uacpi_u8 bytes_skip;

            bytes_skip = op_decode_byte(op_ctx);
            pkg = &item_array_at(&op_ctx->items, pkg_idx)->pkg;

            if (frame->code_offset >= pkg->end)
                op_ctx->pc += bytes_skip;

            break;
        }

        case UACPI_PARSE_OP_IF_NOT_NULL:
        case UACPI_PARSE_OP_IF_NULL: {
            uacpi_u8 idx, bytes_skip;
            bool is_null, skip_if_null;

            idx = op_decode_byte(op_ctx);
            bytes_skip = op_decode_byte(op_ctx);

            is_null = item_array_at(&op_ctx->items, idx)->handle == UACPI_NULL;
            skip_if_null = op == UACPI_PARSE_OP_IF_NOT_NULL;

            if (is_null == skip_if_null)
                op_ctx->pc += bytes_skip;

            break;
        }

        case UACPI_PARSE_OP_IF_EQUALS: {
            uacpi_u8 value, bytes_skip;

            value = op_decode_byte(op_ctx);
            bytes_skip = op_decode_byte(op_ctx);

            if (item->immediate != value)
                op_ctx->pc += bytes_skip;

            break;
        }

        case UACPI_PARSE_OP_JMP: {
            op_ctx->pc = op_decode_byte(op_ctx);
            break;
        }

        case UACPI_PARSE_OP_CREATE_NAMESTRING:
        case UACPI_PARSE_OP_EXISTING_NAMESTRING:
        case UACPI_PARSE_OP_EXISTING_NAMESTRING_OR_NULL: {
            uacpi_size offset = frame->code_offset;
            const uacpi_char *action;
            enum resolve_behavior behavior;

            if (op == UACPI_PARSE_OP_CREATE_NAMESTRING) {
                action = "create";
                behavior = RESOLVE_CREATE_LAST_NAMESEG_FAIL_IF_EXISTS;
            } else {
                action = "resolve";
                behavior = RESOLVE_FAIL_IF_DOESNT_EXIST;
            }

            ret = resolve_name_string(frame, behavior, &item->node);

            if (ret == UACPI_STATUS_NOT_FOUND) {
                uacpi_bool is_ok;

                if (prev_op) {
                    is_ok = op_allows_unresolved(prev_op);
                    is_ok &= op_allows_unresolved(op);
                } else {
                    // This is the only standalone op where we allow unresolved
                    is_ok = op_ctx->op->code == UACPI_AML_OP_ExternalOp;
                }

                if (is_ok)
                    ret = UACPI_STATUS_OK;
            }

            if (uacpi_unlikely_error(ret)) {
                uacpi_char *path = UACPI_NULL;
                uacpi_size length;

                name_string_to_path(frame, offset, &path, &length);
                uacpi_kernel_log(
                    UACPI_LOG_ERROR, "Failed to %s named object '%s': %s\n",
                    action, path ? path : "<unknown>",
                    uacpi_status_to_string(ret)
                );
                uacpi_kernel_free(path);
            }

            break;
        }

        case UACPI_PARSE_OP_INVOKE_HANDLER: {
            uacpi_aml_op code = op_ctx->op->code;
            uacpi_u8 idx;

            if (code <= 0xFF)
                idx = handler_idx_of_op[code];
            else
                idx = handler_idx_of_ext_op[EXT_OP_IDX(code)];

            ret = op_handlers[idx](ctx);
            break;
        }

        case UACPI_PARSE_OP_INSTALL_NAMESPACE_NODE:
            item = item_array_at(&op_ctx->items, op_decode_byte(op_ctx));
            ret = uacpi_node_install(item->node->parent, item->node);

            if (uacpi_likely_success(ret)) {
                if (!frame->method->named_objects_persist) {
                    ret = temp_namespace_node_array_push(
                        &frame->temp_nodes, item->node
                    );
                }

                if (uacpi_likely_success(ret))
                    item->node = UACPI_NULL;
            }

            break;

        case UACPI_PARSE_OP_OBJECT_TRANSFER_TO_PREV:
        case UACPI_PARSE_OP_OBJECT_COPY_TO_PREV: {
            uacpi_object *src;
            struct item *dst;

            if (!ctx->prev_op_ctx)
                break;

            switch (prev_op) {
            case UACPI_PARSE_OP_TERM_ARG_UNWRAP_INTERNAL:
            case UACPI_PARSE_OP_COMPUTATIONAL_DATA:
            case UACPI_PARSE_OP_OPERAND:
                src = uacpi_unwrap_internal_reference(item->obj);

                if (prev_op == UACPI_PARSE_OP_OPERAND)
                    ret = typecheck_operand(ctx->prev_op_ctx, src);
                else if (prev_op == UACPI_PARSE_OP_COMPUTATIONAL_DATA)
                    ret = typecheck_computational_data(ctx->prev_op_ctx, src);

                break;
            case UACPI_PARSE_OP_SUPERNAME:
            case UACPI_PARSE_OP_SUPERNAME_IMPLICIT_DEREF:
            case UACPI_PARSE_OP_SUPERNAME_OR_UNRESOLVED:
                if (prev_op == UACPI_PARSE_OP_SUPERNAME_IMPLICIT_DEREF)
                    src = object_deref_implicit(item->obj);
                else
                    src = item->obj;
                break;

            case UACPI_PARSE_OP_SIMPLE_NAME:
            case UACPI_PARSE_OP_TERM_ARG:
            case UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT:
            case UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT_OR_UNRESOLVED:
            case UACPI_PARSE_OP_TARGET:
                src = item->obj;
                break;

            default:
                EXEC_OP_WARN_1("don't know how to copy/transfer object to %d",
                               prev_op);
                ret = UACPI_STATUS_INVALID_ARGUMENT;
                break;
            }

            if (uacpi_likely_success(ret)) {
                dst = item_array_last(&ctx->prev_op_ctx->items);
                dst->type = ITEM_OBJECT;

                if (op == UACPI_PARSE_OP_OBJECT_TRANSFER_TO_PREV) {
                    dst->obj = src;
                    uacpi_object_ref(dst->obj);
                } else {
                    dst->obj = uacpi_create_object(UACPI_OBJECT_UNINITIALIZED);
                    if (uacpi_unlikely(dst->obj == UACPI_NULL)) {
                        ret = UACPI_STATUS_OUT_OF_MEMORY;
                        break;
                    }

                    ret = uacpi_object_assign(dst->obj, src,
                                              UACPI_ASSIGN_BEHAVIOR_DEEP_COPY);
                }
            }
            break;
        }

        case UACPI_PARSE_OP_STORE_TO_TARGET:
        case UACPI_PARSE_OP_STORE_TO_TARGET_INDIRECT: {
            uacpi_object *dst, *src;

            dst = item_array_at(&op_ctx->items, op_decode_byte(op_ctx))->obj;

            if (op == UACPI_PARSE_OP_STORE_TO_TARGET_INDIRECT) {
                src = item_array_at(&op_ctx->items,
                                    op_decode_byte(op_ctx))->obj;
            } else {
                src = item->obj;
            }

            ret = store_to_target(dst, src);
            break;
        }

        // Nothing to do here, object is allocated automatically
        case UACPI_PARSE_OP_OBJECT_ALLOC:
        case UACPI_PARSE_OP_OBJECT_ALLOC_TYPED:
        case UACPI_PARSE_OP_EMPTY_OBJECT_ALLOC:
            break;

        case UACPI_PARSE_OP_OBJECT_CONVERT_TO_SHALLOW_COPY:
        case UACPI_PARSE_OP_OBJECT_CONVERT_TO_DEEP_COPY: {
            uacpi_object *temp = item->obj;
            enum uacpi_assign_behavior behavior;

            item_array_pop(&op_ctx->items);
            item = item_array_last(&op_ctx->items);

            if (op == UACPI_PARSE_OP_OBJECT_CONVERT_TO_SHALLOW_COPY)
                behavior = UACPI_ASSIGN_BEHAVIOR_SHALLOW_COPY;
            else
                behavior = UACPI_ASSIGN_BEHAVIOR_DEEP_COPY;

            ret = uacpi_object_assign(temp, item->obj, behavior);
            if (uacpi_unlikely_error(ret))
                break;

            uacpi_object_unref(item->obj);
            item->obj = temp;
            break;
        }

        case UACPI_PARSE_OP_DISPATCH_METHOD_CALL: {
            struct uacpi_namespace_node *node;
            struct uacpi_control_method *method;

            node = item_array_at(&op_ctx->items, 0)->node;
            method = uacpi_namespace_node_get_object(node)->method;

            ret = push_new_frame(ctx, &frame);
            if (uacpi_unlikely_error(ret))
                return ret;

            ret = frame_push_args(frame, ctx->cur_op_ctx);
            if (uacpi_unlikely_error(ret))
                return ret;

            ret = frame_setup_base_scope(frame, node, method);
            if (uacpi_unlikely_error(ret))
                return ret;

            ctx->cur_frame = frame;
            ctx->cur_op_ctx = UACPI_NULL;
            ctx->prev_op_ctx = UACPI_NULL;
            ctx->cur_block = code_block_array_last(&ctx->cur_frame->code_blocks);
            return UACPI_STATUS_OK;
        }

        case UACPI_PARSE_OP_CONVERT_NAMESTRING: {
            uacpi_aml_op new_op = UACPI_AML_OP_InternalOpNamedObject;
            uacpi_object *obj;

            if (item->node == UACPI_NULL) {
                if (!op_allows_unresolved(prev_op))
                    ret = UACPI_STATUS_NOT_FOUND;
                break;
            }

            obj = uacpi_namespace_node_get_object(item->node);

            switch (obj->type) {
            case UACPI_OBJECT_METHOD: {
                uacpi_bool should_invoke;

                switch (prev_op) {
                case UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT:
                case UACPI_PARSE_OP_TERM_ARG_OR_NAMED_OBJECT_OR_UNRESOLVED:
                    should_invoke = UACPI_FALSE;
                    break;
                default:
                    should_invoke = !op_wants_supername(prev_op);
                }

                if (!should_invoke)
                    break;

                new_op = UACPI_AML_OP_InternalOpMethodCall0Args;
                new_op += obj->method->args;
                break;
            }

            case UACPI_OBJECT_BUFFER_FIELD:
                if (!op_wants_term_arg_or_operand(prev_op))
                    break;

                switch (buffer_field_get_read_type(&obj->buffer_field)) {
                case UACPI_OBJECT_BUFFER:
                    new_op = UACPI_AML_OP_InternalOpReadFieldAsBuffer;
                    break;
                case UACPI_OBJECT_INTEGER:
                    new_op = UACPI_AML_OP_InternalOpReadFieldAsInteger;
                    break;
                default:
                    ret = UACPI_STATUS_INVALID_ARGUMENT;
                    continue;
                }
                break;
            default:
                break;
            }

            op_ctx->pc = 0;
            op_ctx->op = uacpi_get_op_spec(new_op);
            break;
        }

        default:
            EXEC_OP_WARN_1("unhandled parser op '%d'", op);
            ret = UACPI_STATUS_UNIMPLEMENTED;
            break;
        }
    }
}

static void call_frame_clear(struct call_frame *frame)
{
    uacpi_size i;
    op_context_array_clear(&frame->pending_ops);
    code_block_array_clear(&frame->code_blocks);

    while (temp_namespace_node_array_size(&frame->temp_nodes) != 0) {
        uacpi_namespace_node *node;

        node = *temp_namespace_node_array_last(&frame->temp_nodes);
        uacpi_node_uninstall(node);
        temp_namespace_node_array_pop(&frame->temp_nodes);
    }
    temp_namespace_node_array_clear(&frame->temp_nodes);

    for (i = 0; i < 7; ++i)
        uacpi_object_unref(frame->args[i]);
    for (i = 0; i < 8; ++i)
        uacpi_object_unref(frame->locals[i]);
}

static void execution_context_release(struct execution_context *ctx)
{
    if (ctx->ret)
        uacpi_object_unref(ctx->ret);

    while (call_frame_array_size(&ctx->call_stack) != 0) {
        while (op_context_array_size(&ctx->cur_frame->pending_ops) != 0)
            pop_op(ctx);

        call_frame_clear(call_frame_array_last(&ctx->call_stack));
        call_frame_array_pop(&ctx->call_stack);
    }

    call_frame_array_clear(&ctx->call_stack);
    uacpi_kernel_free(ctx);
}

static void ctx_reload_post_ret(struct execution_context *ctx)
{
    call_frame_clear(ctx->cur_frame);
    call_frame_array_pop(&ctx->call_stack);

    ctx->cur_frame = call_frame_array_last(&ctx->call_stack);
    refresh_ctx_pointers(ctx);
}

uacpi_status uacpi_execute_control_method(uacpi_namespace_node *scope,
                                          uacpi_control_method *method,
                                          uacpi_args *args, uacpi_object **ret)
{
    uacpi_status st = UACPI_STATUS_OK;
    struct execution_context *ctx;

    ctx = uacpi_kernel_calloc(1, sizeof(*ctx));
    if (ctx == UACPI_NULL)
        return UACPI_STATUS_OUT_OF_MEMORY;

    if (ret != UACPI_NULL) {
        ctx->ret = uacpi_create_object(UACPI_OBJECT_UNINITIALIZED);
        if (uacpi_unlikely(ctx->ret == UACPI_NULL)) {
            st = UACPI_STATUS_OUT_OF_MEMORY;
            goto out;
        }
    }

    ctx->cur_method = method;

    ctx->cur_frame = call_frame_array_calloc(&ctx->call_stack);
    if (uacpi_unlikely(ctx->cur_frame == UACPI_NULL)) {
        st = UACPI_STATUS_OUT_OF_MEMORY;
        goto out;
    }

    if (args != UACPI_NULL) {
        uacpi_u8 i;

        if (args->count != method->args) {
            st = UACPI_STATUS_INVALID_ARGUMENT;
            goto out;
        }

        for (i = 0; i < method->args; ++i) {
            ctx->cur_frame->args[i] = args->objects[i];
            uacpi_object_ref(args->objects[i]);
        }
    } else if (method->args) {
        st = UACPI_STATUS_INVALID_ARGUMENT;
        goto out;
    }

    frame_setup_base_scope(ctx->cur_frame, scope, method);
    ctx->cur_block = code_block_array_last(&ctx->cur_frame->code_blocks);

    for (;;) {
        if (!ctx_has_non_preempted_op(ctx)) {
            if (ctx->cur_frame == UACPI_NULL)
                break;

            if (maybe_end_block(ctx))
                continue;

            if (!call_frame_has_code(ctx->cur_frame)) {
                ctx_reload_post_ret(ctx);
                continue;
            }

            st = get_op(ctx);
            if (uacpi_unlikely_error(st))
                goto out;

            trace_op(ctx->cur_op);
        }

        st = exec_op(ctx);
        if (uacpi_unlikely_error(st))
            goto out;

        ctx->skip_else = UACPI_FALSE;
    }

out:
    if (ret && ctx->ret->type != UACPI_OBJECT_UNINITIALIZED) {
        uacpi_object_ref(ctx->ret);
        *ret = ctx->ret;
    }
    execution_context_release(ctx);
    return st;
}

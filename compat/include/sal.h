#pragma once

/* SAL annotations expand to nothing. Add missing names here, alphabetically within their group. */

#define _Analysis_assume_(e)
#define __analysis_assume(e)
#define _Always_(a)
#define _At_(t, a)
#define _At_buffer_(t, i, c, a)
#define _Acquires_exclusive_lock_(l)
#define _Acquires_lock_(l)
#define _Acquires_shared_lock_(l)
#define _Check_return_
#define _Must_inspect_result_
#define _Field_range_(l, h)
#define _Field_size_(s)
#define _Field_size_bytes_(s)
#define _Field_size_bytes_full_(s)
#define _Field_size_bytes_full_opt_(s)
#define _Field_size_bytes_opt_(s)
#define _Field_size_bytes_part_(s, c)
#define _Field_size_full_(s)
#define _Field_size_full_opt_(s)
#define _Field_size_opt_(s)
#define _Field_size_part_(s, c)
#define _Field_z_
#define _Frees_ptr_
#define _Frees_ptr_opt_
#define _Function_class_(n)
#define _Guarded_by_(l)
#define _In_
#define _In_bytecount_(s)
#define _In_count_(s)
#define _In_opt_
#define _In_opt_count_(s)
#define _In_opt_z_
#define _In_range_(l, h)
#define _In_reads_(s)
#define _In_reads_bytes_(s)
#define _In_reads_bytes_opt_(s)
#define _In_reads_opt_(s)
#define _In_reads_opt_z_(s)
#define _In_reads_or_z_(s)
#define _In_reads_z_(s)
#define _In_z_
#define _In_NLS_string_(s)
#define _Inexpressible_(e)
#define _Inout_
#define _Inout_count_(s)
#define _Inout_opt_
#define _Inout_opt_z_
#define _Inout_updates_(s)
#define _Inout_updates_bytes_(s)
#define _Inout_updates_bytes_opt_(s)
#define _Inout_updates_opt_(s)
#define _Inout_updates_z_(s)
#define _Inout_z_
#define _Interlocked_operand_
#define _IRQL_requires_(i)
#define _IRQL_requires_max_(i)
#define _IRQL_requires_min_(i)
#define _Maybenull_
#define _Notnull_
#define _Null_terminated_
#define _NullNull_terminated_
#define _On_failure_(a)
#define _Out_
#define _Out_opt_
#define _Out_range_(l, h)
#define _Out_writes_(s)
#define _Out_writes_all_(s)
#define _Out_writes_all_opt_(s)
#define _Out_writes_bytes_(s)
#define _Out_writes_bytes_all_(s)
#define _Out_writes_bytes_all_opt_(s)
#define _Out_writes_bytes_opt_(s)
#define _Out_writes_bytes_to_(s, c)
#define _Out_writes_bytes_to_opt_(s, c)
#define _Out_writes_opt_(s)
#define _Out_writes_opt_z_(s)
#define _Out_writes_to_(s, c)
#define _Out_writes_to_opt_(s, c)
#define _Out_writes_z_(s)
#define _Outptr_
#define _Outptr_opt_
#define _Outptr_opt_result_buffer_(s)
#define _Outptr_opt_result_bytebuffer_(s)
#define _Outptr_opt_result_maybenull_
#define _Outptr_opt_result_z_
#define _Outptr_result_buffer_(s)
#define _Outptr_result_buffer_maybenull_(s)
#define _Outptr_result_bytebuffer_(s)
#define _Outptr_result_maybenull_
#define _Outptr_result_nullonfailure_
#define _Outptr_result_z_
#define _COM_Outptr_
#define _COM_Outptr_opt_
#define _COM_Outptr_opt_result_maybenull_
#define _COM_Outptr_result_maybenull_
#define _Post_
#define _Post_equal_to_(e)
#define _Post_invalid_
#define _Post_maybenull_
#define _Post_notnull_
#define _Post_ptr_invalid_
#define _Post_readable_byte_size_(s)
#define _Post_satisfies_(e)
#define _Post_writable_byte_size_(s)
#define _Post_z_
#define _Pre_
#define _Pre_maybenull_
#define _Pre_notnull_
#define _Pre_satisfies_(e)
#define _Pre_valid_
#define _Pre_z_
#define _Printf_format_string_
#define _Printf_format_string_params_(n)
#define _Releases_exclusive_lock_(l)
#define _Releases_lock_(l)
#define _Releases_shared_lock_(l)
#define _Requires_lock_held_(l)
#define _Requires_lock_not_held_(l)
#define _Reserved_
#define _Result_nullonfailure_
#define _Result_zeroonfailure_
#define _Ret_
#define _Ret_maybenull_
#define _Ret_notnull_
#define _Ret_range_(l, h)
#define _Ret_writes_(s)
#define _Ret_writes_bytes_(s)
#define _Ret_writes_bytes_maybenull_(s)
#define _Ret_writes_maybenull_(s)
#define _Ret_z_
#define _Return_type_success_(e)
#define _Scanf_format_string_
#define _Success_(e)
#define _Struct_size_bytes_(s)
#define _Translates_Win32_to_HRESULT_(e)
#define _Use_decl_annotations_
#define _When_(c, a)
#define _Writable_bytes_(s)
#define _Writable_elements_(s)
#define _Readable_bytes_(s)
#define _Readable_elements_(s)
#define _Valid_
#define _Deref_out_
#define _Deref_out_opt_
#define _Deref_post_null_
#define _Deref_pre_maybenull_
#define _Deref_out_range_(l, h)
#define _Strict_type_match_
#define _Points_to_data_
#define _Literal_
#define _Notliteral_
#define _Kernel_float_used_
#define _Pre_defensive_
#define _Post_defensive_
#define _Ret_opt_
#define _Out_z_cap_(s)
#define _Out_cap_(s)
#define _In_bytecount_c_(s)
#define _Deref_post_z_
#define _Pre_null_
#define _Pre_opt_valid_
#define _Pre_readable_size_(s)
#define _Pre_writable_size_(s)

#define __in
#define __in_opt
#define __out
#define __out_opt
#define __inout
#define __inout_opt
#define __in_bcount(s)
#define __in_ecount(s)
#define __out_bcount(s)
#define __out_ecount(s)
#define __reserved
#define __callback
#define __checkReturn
#define __success(e)

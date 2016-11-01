/*
 * Copyright 2015 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */

#include <backup.h>
#include <enc_text.h>
#include <utils.h>

///
/// Maps a BLOB type to its one-character label.
///
/// @param type  The BLOB type.
///
/// @result      The one-character label, -1 if the given BLOB type is invalid.
///
static int32_t
text_bytes_type_to_label(as_bytes_type type)
{
	static as_bytes_type types[] = {
		AS_BYTES_BLOB,
		AS_BYTES_JAVA,
		AS_BYTES_CSHARP,
		AS_BYTES_PYTHON,
		AS_BYTES_RUBY,
		AS_BYTES_PHP,
		AS_BYTES_ERLANG,
		AS_BYTES_MAP,
		AS_BYTES_LIST,
		AS_BYTES_LDT
	};

	static char labels[] = {
		'B', 'J', 'C', 'P', 'R', 'H', 'E', 'M', 'L', 'U'
	};

	for (size_t i = 0; i < sizeof types / sizeof types[0]; ++i) {
		if (type == types[i]) {
			return labels[i];
		}
	}

	err("Invalid bytes type %d", (int32_t)type);
	return -1;
}

///
/// Writes a signed 64-bit integer to the backup file.
///
/// @param bytes    Increased by the number of bytes written to the backup file.
/// @param fd       The file descriptor of the backup file.
/// @param prefix1  The first string prefix.
/// @param prefix2  The second string prefix.
/// @param val      The integer value to be written.
///
/// @result         `true`, if successful.
///
static bool
text_output_integer(uint64_t *bytes, FILE *fd, const char *prefix1, const char *prefix2,
		as_val *val)
{
	as_integer *v = as_integer_fromval(val);

	//if (fprintf_bytes(bytes, fd, "%s%s %" PRId64 "\n", prefix1, prefix2, v->value) < 0) {
	//if (fprintf_bytes(bytes, fd, "|%s#%" PRId64 "|", prefix2, v->value) < 0) {
	if (fprintf_bytes(bytes, fd, "%" PRId64, v->value) < 0) {
		err_code("Error while writing integer to backup file");
		return false;
	}

	return true;
}

///
/// Writes a double-precision floating-point value to the backup file.
///
/// Using 17 significant digits makes sure that we read back the same value that we wrote.
///
/// @param bytes    Increased by the number of bytes written to the backup file.
/// @param fd       The file descriptor of the backup file.
/// @param prefix1  The first string prefix.
/// @param prefix2  The second string prefix.
/// @param val      The floating point value to be written.
///
/// @result         `true`, if successful.
///
static bool
text_output_double(uint64_t *bytes, FILE *fd, const char *prefix1, const char *prefix2,
		as_val *val)
{
	as_double *v = as_double_fromval(val);

	//if (fprintf_bytes(bytes, fd, "|%s#%.17g|", prefix2, v->value) < 0) {
	if (fprintf_bytes(bytes, fd, "%.17g", v->value) < 0) {
		err_code("Error while writing double to backup file");
		return false;
	}

	return true;
}

///
/// Writes a BLOB to the backup file.
///
/// @param bytes    Increased by the number of bytes written to the backup file.
/// @param fd       The file descriptor of the backup file.
/// @param prefix1  The first string prefix.
/// @param prefix2  The second string prefix.
/// @param buffer   The data of the BLOB to be written.
/// @param size     The size of the BLOB to be written.
///
/// @result         `true`, if successful.
///

static bool
text_output_data_1(uint64_t *bytes, FILE *fd, const char *prefix2, void *buffer, size_t size)
{
	if (fwrite_bytes(bytes, buffer, size, 1, fd) != 1) {
		err_code("Error while writing data to backup file [2]");
		return false;
	}
	return true;
}

static bool
text_output_data(uint64_t *bytes, FILE *fd, const char *prefix2, void *buffer, size_t size)
{
	if (strcmp(prefix2, "") == 0)
		return true;

	//if (fprintf_bytes(bytes, fd, "|%s#", prefix2) < 0) {
	//	err_code("Error while writing data to backup file [1]");
	//	return false;
	//}

	if (fwrite_bytes(bytes, buffer, size, 1, fd) != 1) {
		err_code("Error while writing data to backup file [2]");
		return false;
	}
	//fprintf_bytes(bytes, fd, "%s", "|");


	//if (fprintf_bytes(bytes, fd, ",") < 0) {
	//	err_code("Error while writing data to backup file [3]");
	//	return false;
	//}


	return true;
}

///
/// Writes an encoded BLOB to the backup file.
///
/// @param bytes    Increased by the number of bytes written to the backup file.
/// @param fd       The file descriptor of the backup file.
/// @param prefix1  The first string prefix.
/// @param prefix2  The second string prefix.
/// @param buffer   The data of the BLOB to be written.
/// @param size     The size of the BLOB to be written.
///
/// @result         `true`, if successful.
///
static bool
text_output_data_enc(uint64_t *bytes, FILE *fd, const char *prefix2, void *buffer, uint32_t size)
{
	uint32_t enc_size = cf_b64_encoded_len(size);

	if (enc_size < size) {
		err("Encoded data too long (%u vs. %u bytes)", enc_size, size);
		return false;
	}

	char *enc = buffer_init(enc_size);
	cf_b64_encode(buffer, size, enc);

	if (!text_output_data(bytes, fd, prefix2, enc, enc_size)) {
		buffer_free(enc, enc_size);
		return false;
	}

	buffer_free(enc, enc_size);
	return true;
}

///
/// Writes a string to the backup file.
///
/// @param bytes    Increased by the number of bytes written to the backup file.
/// @param fd       The file descriptor of the backup file.
/// @param prefix1  The first string prefix.
/// @param prefix2  The second string prefix.
/// @param val      The string to be written.
///
/// @result         `true`, if successful.
///
void escape_and_quote_str (char *str, char *estr)
{
	int i = 0;
	estr[i++] = '"';
	while (*str)
	{
		if (*str < 32 || *str > 126)
			estr[i] = ' ';
		else if (*str == '"')
		{
			estr[i++] = '\\';
			estr[i] = '"';
		}
		else
			estr[i] = *str;
		i++;
		str++;
	}
	estr[i++] = '"';
	estr[i] = 0;
//	printf ("ESTR=%s|\n", estr);
//	exit(0);
}

static bool
text_output_string(uint64_t *bytes, FILE *fd, const char *prefix1, const char *prefix2, as_val *val)
{
	as_string *v = as_string_fromval(val);
	char *estr = (char *) malloc (strlen(v->value) * 2 + 3);
	escape_and_quote_str (v->value, estr);
	//escape_and_quote_str ("THIS \"IS\" TEST", estr);
	bool rv = text_output_data(bytes, fd, prefix2, estr, strlen(estr));

	free (estr);
	return rv;
}

///
/// Writes a geojson to the backup file.
///
/// @param bytes    Increased by the number of bytes written to the backup file.
/// @param fd       The file descriptor of the backup file.
/// @param prefix1  The first string prefix.
/// @param prefix2  The second string prefix.
/// @param val      The geojson to be written.
///
/// @result         `true`, if successful.
///
static bool
text_output_geojson(uint64_t *bytes, FILE *fd, const char *prefix1, const char *prefix2, as_val *val)
{
	as_geojson *v = as_geojson_fromval(val);
	return text_output_data(bytes, fd, prefix2, v->value, v->len);
}

///
/// Writes a bytes value to the backup file.
///
/// @param bytes    Increased by the number of bytes written to the backup file.
/// @param fd       The file descriptor of the backup file.
/// @param compact  Indicates compact mode.
/// @param prefix1  The first string prefix.
/// @param prefix2  The second string prefix.
/// @param val      The bytes value to be written.
///
/// @result         `true`, if successful.
///
static bool
text_output_bytes(uint64_t *bytes, FILE *fd, bool compact, const char *prefix1, const char *prefix2,
		as_val *val)
{
	as_bytes *v = as_bytes_fromval(val);
	return compact ?
			text_output_data(bytes, fd, prefix2, v->value, v->size) :
			text_output_data_enc(bytes, fd, prefix2, v->value, v->size);
}

///
/// Writes a key to the backup file.
///
/// @param bytes    Increased by the number of bytes written to the backup file.
/// @param fd       The file descriptor of the backup file.
/// @param compact  Indicates compact mode.
/// @param key      The key to be written.
///
/// @result         `true`, if successful.
///
static bool
text_output_key(uint64_t *bytes, FILE *fd, bool compact, as_val *key)
{
	switch (key->type) {
	case AS_INTEGER:
		return text_output_integer(bytes, fd, "+ k I", "", key);

	case AS_DOUBLE:
		return text_output_double(bytes, fd, "+ k D", "", key);

	case AS_STRING:
		return text_output_string(bytes, fd, "+ k S", "", key);

	case AS_BYTES:
		return compact ?
			text_output_bytes(bytes, fd, true, "+ k B!", "", key) :
			text_output_bytes(bytes, fd, false, "+ k B", "", key);

	default:
		err("Invalid key type %d", (int32_t)key->type);
		return false;
	}
}

///
/// Writes a bin to the backup file.
///
/// @param bytes     Increased by the number of bytes written to the backup file.
/// @param fd        The file descriptor of the backup file.
/// @param compact   Indicates compact mode.
/// @param bin_name  The name of the bin to be written.
/// @param val       The bin value to be written.
///
/// @result         `true`, if successful.
///
static bool
text_output_value(uint64_t *bytes, FILE *fd, bool compact, const char *bin_name, as_val *val)
{
	if (val == NULL || val->type == AS_NIL)
	{
		//if (fprintf_bytes(bytes, fd, "|%s#|", bin_name) < 0)
		//if (fprintf_bytes(bytes, fd, ",") < 0)
		//{
		//	err_code("Error while writing NIL value to backup file");
		//	return false;
		//}

		return true;
	}

	switch (val->type) {
	case AS_INTEGER:
		return text_output_integer(bytes, fd, "- I ", bin_name, val);

	case AS_DOUBLE:
		return text_output_double(bytes, fd, "- D ", bin_name, val);

	case AS_STRING:
		return text_output_string(bytes, fd, "- S ", bin_name, val);

	case AS_GEOJSON:
		return text_output_geojson(bytes, fd, "- G ", bin_name, val);

	case AS_BYTES: {
		int32_t type = text_bytes_type_to_label(as_bytes_fromval(val)->type);

		if (type < 0) {
			return false;
		}

		if (compact) {
			char prefix[6] = "-  ! ";
			prefix[2] = (char)type;
			return text_output_bytes(bytes, fd, true, prefix, bin_name, val);
		}

		char prefix[5] = "-   ";
		prefix[2] = (char)type;
		return text_output_bytes(bytes, fd, false, prefix, bin_name, val);
	}

	case AS_LIST:
		err("Unexpected value of type list");
		return false;

	case AS_MAP:
		err("Unexpected value of type map");
		return false;

	default:
		err("Invalid value type %d", (int32_t)val->type);
		return false;
	}
}

///
/// Part of the interface exposed by the text backup file format encoder.
///
/// See backup_encoder.put_record for details.
///
bool
text_put_record(uint64_t *bytes, FILE *fd, bool compact, const as_record *rec)
{
	uint32_t enc_size = cf_b64_encoded_len(sizeof (as_digest_value)) + 1;
	char *enc = alloca(enc_size);
	cf_b64_encode(rec->key.digest.value, sizeof (as_digest_value), enc);
	enc[enc_size - 1] = 0;

	// map -1 TTL (= never expire) to 0; restore maps it back
	uint32_t expire = rec->ttl == (uint32_t)-1 ? 0 : (uint32_t)cf_secs_since_clepoch() + rec->ttl;

	if (rec->key.valuep != NULL &&
			!text_output_key(bytes, fd, compact, (as_val *)rec->key.valuep)) {
		err("Error while writing record key");
		return false;
	}

/*
	if (fprintf_bytes(bytes, fd, "+ n %s\n+ d %s\n", escape(rec->key.ns), enc) < 0) {
		err_code("Error while writing record meta data to backup file [1]");
		return false;
	}

	if (rec->key.set[0] != 0 && fprintf_bytes(bytes, fd, "+ s %s\n", escape(rec->key.set)) < 0) {
		err_code("Error while writing record meta data to backup file [2]");
		return false;
	}

	int32_t bin_off = 0;

	for (int32_t i = 0; i < rec->bins.size; ++i) {
		if (strcmp(rec->bins.entries[i].name, "LDTCONTROLBIN") == 0) {
			bin_off = -1;
			break;
		}
	}

	if (fprintf_bytes(bytes, fd, "+ g %d\n+ t %u\n+ b %d\n", rec->gen, expire,
			rec->bins.size + bin_off) < 0) {
		err_code("Error while writing record meta data to backup file [3]");
		return false;
	}
*/

	/* for bins passed on command line */
	for (uint32_t j = 0; j < cmdbins.bincount; ++j) {

		bool bin_present_in_data = false;
		as_bin *bin;
		char *userbinname = cmdbins.inputbins[j];
		//printf ("userbinname= %s\n", userbinname);
		for (int32_t i = 0; i < rec->bins.size; ++i) {
			bin = &rec->bins.entries[i];

			if (strcmp(bin->name, "LDTCONTROLBIN") == 0 || strcmp(bin->name, "") == 0) {
				continue;
			}

			if (strcmp (userbinname, bin->name) != 0)
				continue;
			else
			{
				bin_present_in_data = true;
				break;
			}
		}
		if (bin_present_in_data == false)
		{
			if (!text_output_value(bytes, fd, compact, escape(userbinname), NULL)) {
				err("Error while writing EMPTY record bin %s", userbinname);
				return false;
			}
			if (j<(cmdbins.bincount-1))
				text_output_data_1(bytes, fd, "", ",", 1);
		}
		else if (bin_present_in_data)
		{
			if (!text_output_value(bytes, fd, compact, escape(bin->name), (as_val *)bin->valuep)) {
				err("Error while writing record bin %s", bin->name);
				return false;
			}
			if (j<(cmdbins.bincount-1))
				text_output_data_1(bytes, fd, "", ",", 1);
		}
		bin_present_in_data = false;
		//printf ("checking next userbin\n");
	}
	//printf ("checked all userbins\n");
	text_output_data_1(bytes, fd, "", "\n", 1);

	return true;
}

///
/// Maps a UDF type to its one-character label.
///
/// @param type  The UDF type.
///
/// @result      The one-character label, -1 if the given UDF type is invalid.
///
static int32_t
text_udf_type_to_label(as_udf_type type)
{
	if (type == AS_UDF_TYPE_LUA) {
		return 'L';
	}

	err("Invalid UDF type %d", (int32_t)type);
	return -1;
}

///
/// Part of the interface exposed by the text backup file format encoder.
///
/// See backup_encoder.put_udf_file for details.
///
bool
text_put_udf_file(uint64_t *bytes, FILE *fd, const as_udf_file *file)
{
	int32_t type = text_udf_type_to_label(file->type);

	if (type < 0) {
		return false;
	}

	if (fprintf_bytes(bytes, fd, GLOBAL_PREFIX "u %c %s %u ", (char)type, escape(file->name),
			file->content.size) < 0) {
		err_code("Error while writing UDF function to backup file [1]");
		return false;
	}

	if (fwrite_bytes(bytes, file->content.bytes, file->content.size, 1, fd) != 1) {
		err_code("Error while writing UDF function to backup file [2]");
		return false;
	}

	if (fprintf_bytes(bytes, fd, "\n") < 0) {
		err_code("Error while writing UDF function to backup file [3]");
		return false;
	}

	return true;
}

///
/// Maps a secondary index type to its one-character label.
///
/// @param type  The secondary index type.
///
/// @result      The one-character label.
///
static int32_t
text_index_type_to_label(index_type type)
{
	return "INLKV"[(int32_t)type];
}

///
/// Maps a path data type to its one-character label.
///
/// @param type  The path data type.
///
/// @result      The one-character label.
///
static int32_t
text_path_type_to_label(path_type type)
{
	return "ISNG"[(int32_t)type];
}

///
/// Part of the interface exposed by the text backup file format encoder.
///
/// See backup_encoder.put_secondary_index for details.
///
bool
text_put_secondary_index(uint64_t *bytes, FILE *fd, const index_param *index)
{
	if (fprintf_bytes(bytes, fd, GLOBAL_PREFIX "i %s %s %s %c %u",
			escape(index->ns), index->set != NULL ? escape(index->set) : "", escape(index->name),
			text_index_type_to_label(index->type), index->path_vec.size) < 0) {
		err_code("Error while writing secondary index to backup file [1]");
		return false;
	}

	for (uint32_t i = 0; i < index->path_vec.size; ++i) {
		path_param *path = as_vector_get((as_vector *)&index->path_vec, i);

		if (fprintf_bytes(bytes, fd, " %s %c", escape(path->path),
				text_path_type_to_label(path->type)) < 0) {
			err_code("Error while writing secondary index to backup file [2]");
			return false;
		}
	}

	if (fprintf_bytes(bytes, fd, "\n") < 0) {
		err_code("Error while writing secondary index to backup file [3]");
		return false;
	}

	return true;
}

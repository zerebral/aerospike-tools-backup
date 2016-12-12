## Aerospike Backup Tools

This is the developer documentation. For user documentation, please consult http://www.aerospike.com/docs/tools/backup.

## Building

Building the backup tools requires the source code of the Aerospike C client. Please clone it from GitHub.

    git clone https://github.com/aerospike/aerospike-client-c.

Then build the client.

    cd aerospike-client-c
    make
    cd ..

Then set the `CLIENTREPO` environment variable to point to the `aerospike-client-c` directory. The backup tools build process uses that variable to find the client code.

    export CLIENTREPO=$(pwd)/aerospike-client-c

Now clone the source code of the Aerospike backup tools from GitHub.

    git clone https://github.com/aerospike/aerospike-tools-backup

Then build the backup tools and generate the Doxygen documentation.

    cd aerospike-tools-backup
    make
    make docs

This gives you three binaries in the `bin` subdirectory -- `asbackup`, `asrestore`, and `fill` -- as well as the Doxygen HTML documentation in `docs`. Open `docs/index.html` to access the generated documentation.

In order to run the tests that come with the code, you need `asd` installed in `/usr/bin`. The tests invoke `asd` with a separate configuration file, so that your regular database environment remains untouched.

Please make sure that your `python` command is Python 2 and that you have `virtualenv` installed. By default, the tests run `asbackup` and `asrestore` under the Valgrind memory checker. If you don't have the `valgrind` command, please change `USE_VALGRIND` in `test/lib.py` to `False`. Then run the tests.

    make tests

This creates a virtual Python environment in a new subdirectory (`env`), activates it, and installs the Python packages required by the tests. Then the actual tests run.

## Backup Source Code

Let's take a quick look at the overall structure of the `asbackup` source code, at `src/backup.c`. The code does the following, starting at `main()`.

  * Parse command line options into local variables or, if they need to be passed to a worker thread later, into a `backup_config` structure.
  * Initialize an Aerospike client and connect it to the cluster to be backed up.
  * Create the counter thread, which starts at `counter_thread_func()`. That's the thread that outputs the status and counter updates during the backup, among other things.
  * When backing up to a single file (`--output-file` option, as opposed to backing up to a directory using `--directory`), create and open that backup file.
  * Populate a `backup_thread_args` structure for each node to be backed up and submit it to the `job_queue` queue. Note two things:
    - Only one of the `backup_thread_args` structures gets its `first` member set to `true`.
    - When backing up to a single file, the `shared_fd` member gets the file handle of the created backup file (and `NULL` otherwise).
  * Spawn backup worker threads, which start at `backup_thread_func()`. There's one of those for each cluster node to be backed up.
  * Wait for all backup worker threads to finish.
  * When backing up to a single file, close that file.
  * Shut down the Aerospike client.

Let's now look at what the worker threads do, starting at `backup_thread_func()`.

  * Pop a `backup_thread_args` structure off the job queue. The job queue contains exactly one of those for each thread.
  * Initialize a `per_node_context` structure. That's where all the data local to a worker thread is kept. Some of the data is initialized from the `backup_thread_args` structure. In particular, when backing up to a single file, the `fd` member of the `per_node_context` structure is initialized from the `shared_fd` member of the `backup_thread_args` structure. In that way, all backup threads share the same backup file handle.
  * When backing up to a directory, open an exclusive backup file for the worker thread by invoking `open_dir_file()`.
  * If the backup thread is the single thread that has `first` set to `true` in its `backup_thread_args` structure, store secondary index definitions by invoking `process_secondary_indexes()`, and store UDF files by invoking `process_udfs()`. So, this work is done by a single thread, and that thread is chosen by setting its `first` member to `true`.
  * All other threads wait for the chosen thread to finish its secondary index and UDF file work by invoking `wait_one_shot()`. The chosen thread signals completion by invoking `signal_one_shot()`.
  * Initiate backup of records by invoking `aerospike_scan_node()` with `scan_callback()` as the callback function that gets invoked for each record in the namespace to be backed up. From here on, all worker threads work in parallel.

Let's now look at what the callback function, `scan_callback()`, does.

  * When backing up to a directory and the current backup file of a worker thread has grown beyond its maximal size, switch to a new backup file by invoking `close_dir_file()` for the old and `open_dir_file()` for the new backup file.
  * When backing up to a single file, acquire the file lock by invoking `safe_lock()`. As all worker threads share the same backup file, we can only allow one thread to write at a time.
  * Invoke the `put_record()` function of the backup encoder for the current record. The encoder implements the backup file format by taking record information and serializing it to the backup file. Its code is in `src/enc_text.c`, its interface in `include/enc_text.h`. Besides `put_record()`, the interface contains `put_secondary_index()` and `put_udf_file()`, which are used to store secondary index definitions and UDF files in a backup file.
  * When backing up to a single file, release the file lock.

## Restore Source Code

Let's now take a quick look at the overall structure of the `asrestore` source code, at `src/restore.c`. The code does the following, starting at `main()`.

  * Parse command line options into local variables or, if they need to be passed to a worker thread later, into a `restore_config` structure.
  * Initialize an Aerospike client and connect it to the cluster to be restored.
  * Create the counter thread, which starts at `counter_thread_func()`. That's the thread that outputs the status and counter updates during the restore, among other things.
  * When restoring from a directory (`--directory` option, as opposed to restoring from a single file using `--input-file`), collect all backup files from that directory. Then go through the backup files, find the one that has the secondary index definitions and UDF files in it, and parse that information by invoking `get_indexes_and_udfs()`. Then populate one `restore_thread_args` structure for each backup file and submit it to the `job_queue` queue.
  * When restoring from a single file, open that file and populate the `shared_fd` member of the `restore_thread_args` structure with the file handle of that shared backup file. Then parse the secondary index definitions and UDF files from that file by invoking `get_indexes_and_udfs()`. Finally, submit one `restore_thread_args` structure for each worker thread to the job queue.
  * Restore the UDF files to the cluster by invoking `restore_udfs()`.
  * When secondary indexes are to be restored before any records, invoke `restore_indexes()` to create them.
  * Create the restore worker threads, which start at `restore_thread_func()`.
  * Wait for all restore worker threads to finish.
  * When secondary indexes are to be restore after all records, invoke `restore_indexes()` to create them.
  * When restoring from a single file, close that file.
  * Shut down the Aerospike client.

Let's now look at what the worker threads do, starting at `restore_thread_func()`. The code is pretty similar in structure to its counterpart in `asbackup`.

  * Pop a `restore_thread_args` structure off the job queue.
  * Initialize a `per_thread_context` structure. That's where all the data local to a worker thread is kept. Some of the data is initialized from the `restore_thread_args` structure. In particular, when restoring from a single file, the `fd` member of the `per_thread_context` structure is initialized from the `shared_fd` member of the `restore_thread_args` structure. In that way, all restore threads share the same backup file handle.
  * When restoring from a directory, open an exclusive backup file for the worker thread by invoking `open_file()`.
  * Set up the write policy, depending on the command line arguments given by the user.
  * When restoring from a single file, acquire the file lock by invoking `safe_lock()`. As all worker threads read from the same backup file, we can only allow one thread to read at a time.
  * Invoke the `parse()` function of the backup decoder to read the next record. The decoder is the counterpart to the encoder in `asbackup`. It implements the backup file format by deserializing record information from the the backup file. Its code is in `src/dec_text.c`, its interface in `include/dec_text.h`.
  * When backing up to a single file, release the file lock.
  * Invoke `aerospike_key_put()` to store the current record in the cluster.
  * When LDT list bins are involved, things get a little less linear: the decoder calls back into `restore.c`. `prepare_ldt_record()` is invoked when the first LDT list bin of the current record is encountered. It simulates a write policy for LDT list bins. `add_ldt_value()` is invoked for each LDT list element to be added to an LDT list bin. The function batches the LDT list elements and then collectively adds the batched elements to the LDT bin using `aerospike_llist_update_all()`. When there aren't any more elements for the current LDT bin, `NULL` is passed as the element pointer, which makes the function clean up its data structures and prepare itself for the next LDT bin.

For more detailed information, please generate the documentation (`make docs`) and open `docs/index.html`.

## Fill Utility

`fill` is a small utility that populates a database with test data. It fills records with pseudo-random data according to record specifications and adds them to a given set in a given namespace.

### Record Specifications

A record specification defines the bins of a generated record, i.e., how many there are and what type of data they contain: a 50-character string, a 100-element list of integers, or something more deeply nested, such as a 100-element list of 50-element maps that map integer keys to 500-character string values.

The available record specifications are read from a file, `spec.txt` by default. The format of the file is slightly Lisp-like. Each record specification has the following general structure.

    (record "{spec-id}"
        {bin-count-1} {bin-type-1}
        {bin-count-2} {bin-type-2}
        ...)

This declares a record specification that can be accessed under the unique identifier `{spec-id}`. It defines a record that has `{bin-count-1}`-many bins with data of type `{bin-type-1}`, `{bin-count-2}`-many bins with data of type `{bin-type-2}`, etc.

Accordingly, the `{bin-count-x}` placeholders are just integer values. The `{bin-type-x}` placeholders, on the other hand, are a little more complex. They have to be able to describe nested data types. They have one of six forms.

| `{bin-type-x}` | Semantics |
|----------------|-----------|
| `(integer)`    | A 64-bit integer value. |
| `(double)`     | A 64-bit floating-point value |
| `(string {length})` | A string value of the given length. |
| `(list {length} {element-type})` | A list of the given length, whose elements are of the given type. This type can then again have one of these six forms. |
| `(map {size} {key-type} {value-type})` | A map of the given size, whose keys and values are of the given types. These types can then again have one of these six forms. |
| `(llist {length} {element-type})` | The LDT equivalent to `(list ...)`. |
| `(lmap {size} {key-type} {value-type})` | The LDT equivalent to `(map ...)`. |

Let's reconsider the above examples: a 50-character string, a 100-element list of integers, and a 100-element list of 50-element maps that map integer keys to 500-character string values. Let's specify a record that has 1, 3, and 5 bins of those types, respectively.

    (record "example"
        1 (string 50)
        3 (list 100 (integer))
        5 (list 100 (map 50 (integer) (string 500))))

### Fill Source Code

The specification file is parsed by a Ragel (http://www.colm.net/open-source/ragel/) parser. The state machine for the parser is in `src/spec.rl`. Ragel automatically generates the C parser code from this file. Not everybody has Ragel installed, so the auto-generated C file, `src/spec.c`, is included in the Git repository. If you want to re-generate `spec.c` from `spec.rl`, do the following.

    make ragel

The parser interfaces with the rest of the code via a single function, parse(). This parses the specification file into a linked list of record specifications (@ref rec_node). Each record specification points to a linked list of bin specifications (@ref bin_node), each of which, in turn, says how many bins to add to the record and with which data type. The data type is given by a tree of @ref type_node. See the documentation of spec.h and the `struct` types declared there for more information.

In its most basic form, the `fill` command could be invoked as follows, for example.

    fill test-ns test-set 1000 test-spec-1 2000 test-spec-2 3000 test-spec-3

This would add a total of 6,000 records to set `test-set` in namespace `test-ns`: 1,000 records based on `test-spec-1`, 2,000 records based on `test-spec-2`, and 3,000 records based on `test-spec-3`.

The three (count, record specification) pairs -- (1000, "test-spec-1"), (2000, "test-spec-2"), (3000, "test-spec-3") -- are parsed into a linked list of _fill jobs_ (@ref job_node). The code then iterates through this list and invokes fill() for each job.

The fill() function fires up the worker threads and then just sits there and prints progress information until the worker threads are all done.

The worker threads start at the fill_worker() function. This function generates records according to the given record specification (create_record()), generates a key (init_key()), and puts the generated record in the given set in the given namespace using the generated key. The individual bin values are created by generate(), which recurses in the case of nested data types. Please consult the documentation for fill.c for more details on the code.

The following options to `fill` are probably non-obvious.

| Option             | Effect |
|--------------------|--------|
| `-k {key-type}`    | By default, we randomly pick an integer key, a string key, or a bytes key for each record. Specifying `integer`, `string`, or `bytes` as the `{key-type}` forces a random key of the given type to be created instead. |
| `-c {tps-ceiling}` | Limits the total number of records put per second by the `fill` tool (TPS) to the given ceiling. Handy to prevent server overload. |
| `-b`               | Enables benchmark mode, which speeds up the `fill` tool. In benchmark mode we generate just one single record for a fill job and repeatedly put this same record with different keys; all records of a job thus contain identical data. Without benchmark mode, each record to be put is re-generated from scratch, which results in unique data in each record. |
| `-z`               | Enables fuzzing. Fuzzing uses random junk data for bin names, string and BLOB bin values, etc. in order to try to trip the backup file format parser. |

## Backup File Format

The output produced will be CSV files that will 
- contain data for the bins specified to the asbackup command with the --bin-list command line option
- In the order in which the bins a mentioned with the --bin-list command line option
- Each field value will be enclosed in double quotes
- In a bin has no value then a coulmn without any value will be created for it
- Any double quotes within a bin value will be escaped

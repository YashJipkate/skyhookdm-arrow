# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# cython: language_level = 3

from pyarrow._dataset cimport Dataset
from pyarrow.lib cimport *
from pyarrow.lib import frombytes, tobytes
from pyarrow.includes.common cimport *
from pyarrow.includes.libarrow cimport *
from pyarrow.includes.libarrow_dataset cimport *
from pyarrow.lib cimport _Weakrefable

cdef extern from "arrow/dataset/file_rados_parquet.h" \
        namespace "arrow::dataset" nogil:
    cdef cppclass CRadosParquetFileFormat \
        "arrow::dataset::RadosParquetFileFormat"(
            CFileFormat):
        CRadosParquetFileFormat(
            c_string ceph_config_path,
            c_string data_pool,
            c_string user_name,
            c_string cluster_name
        )

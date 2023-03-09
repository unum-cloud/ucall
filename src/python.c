/**
 * @file python.c
 * @author Ashot Vardanian
 * @date 2023-01-30
 * @copyright Copyright (c) 2023
 *
 * @brief Pure CPython bindings for UJRPC.
 *
 * @see Reading Materials
 * https://pythoncapi.readthedocs.io/type_object.html
 * https://numpy.org/doc/stable/reference/c-api/types-and-structures.html
 * https://pythonextensionpatterns.readthedocs.io/en/latest/refcount.html
 * https://docs.python.org/3/extending/newtypes_tutorial.html#adding-data-and-methods-to-the-basic-example
 */
#include <time.h>

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <turbob64.h>

#include "helpers/py_to_json.h"
#include "ujrpc/ujrpc.h"

#define stringify_value_m(a) stringify_m(a)
#define stringify_m(a) #a
#define concat_m(A, B) A##B
#define macro_concat_m(A, B) concat_m(A, B)
#define pyinit_f_m macro_concat_m(PyInit_, UKV_PYTHON_MODULE_NAME)

#define get_attr_safe_m(name, obj, attr)                                                                               \
    PyObject* name = PyObject_GetAttrString(obj, attr);                                                                \
    if (!name) {                                                                                                       \
        PyErr_SetString(PyExc_TypeError, "Failed to extract attribute: " attr);                                        \
        return -1;                                                                                                     \
    }

typedef enum {
    POSITIONAL_ONLY,       //
    POSITIONAL_OR_KEYWORD, //
    VAR_POSITIONAL,        //
    KEYWORD_ONLY,          //
    VAR_KEYWORD            //
} py_param_kind_t;

typedef struct {
    const char* name;     // Name or NULL
    Py_ssize_t name_len;  // Name Length
    PyObject* value;      // Any or NULL
    PyTypeObject* type;   // Type or NULL
    py_param_kind_t kind; // Kind
} py_param_t;

typedef struct {
    py_param_t* u_params;
    size_t params_cnt;
    PyObject* callable;
} py_wrapper_t;

typedef struct {
    PyObject_HEAD;
    ujrpc_config_t config;
    ujrpc_server_t server;
    size_t count_threads;
    py_wrapper_t* wrappers;
    size_t wrapper_capacity;
    size_t count_added;
} py_server_t;

static int prepare_wrapper(PyObject* callable, py_wrapper_t* wrap) {
    get_attr_safe_m(func_code, callable, "__code__");
    get_attr_safe_m(arg_names, func_code, "co_varnames");
    get_attr_safe_m(co_flags, func_code, "co_flags");
    get_attr_safe_m(co_argcount, func_code, "co_argcount");
    get_attr_safe_m(co_posonlyargcount, func_code, "co_posonlyargcount");
    get_attr_safe_m(co_kwonlyargcount, func_code, "co_kwonlyargcount");
    get_attr_safe_m(varnames, func_code, "co_varnames");

    long flags = PyLong_AsLong(co_flags);
    long pos_count = PyLong_AsLong(co_argcount);
    long posonly_count = PyLong_AsLong(co_posonlyargcount);
    long keyword_only_count = PyLong_AsLong(co_kwonlyargcount);
    long pos_default_count = 0;

    PyObject* annotations = PyFunction_GetAnnotations(callable); // Dict

    get_attr_safe_m(defaults, callable, "__defaults__");
    get_attr_safe_m(kwdefaults, callable, "__kwdefaults__");

    if (PyTuple_CheckExact(defaults))
        pos_default_count = PyTuple_Size(defaults);

    if (PyMethod_Check(callable)) {
        // When this is a class method...
        // TODO: What?
    }

    long non_default_count = pos_count - pos_default_count;
    long posonly_left = posonly_count;

    // if (posonly_count != pos_count) {
    //     PyErr_SetString(PyExc_TypeError, "Strictly positional or Keyword arguments are allowed.");
    //     return -1;
    // }

    long total_params = pos_count + (flags & CO_VARARGS) + keyword_only_count + (flags & CO_VARKEYWORDS);
    py_param_t* parameters = (py_param_t*)malloc(total_params * sizeof(py_param_t));

    size_t param_i = 0;

    // Non-keyword-only parameters w/o defaults.
    for (Py_ssize_t i = 0; i < non_default_count; ++i) {
        py_param_t param;
        param.name = PyUnicode_AsUTF8AndSize(PyTuple_GetItem(arg_names, i), &param.name_len);
        param.value = NULL;
        param.type = (PyTypeObject*)PyDict_GetItemString(annotations, param.name);
        param.kind = posonly_left-- > 0 ? POSITIONAL_ONLY : POSITIONAL_OR_KEYWORD;
        parameters[param_i++] = param;
    }

    //... w / defaults.
    for (Py_ssize_t i = non_default_count; i < pos_count; ++i) {
        py_param_t param;
        param.name = PyUnicode_AsUTF8AndSize(PyTuple_GetItem(arg_names, i), &param.name_len);
        param.value = PyTuple_GetItem(defaults, i - non_default_count);
        param.type = (PyTypeObject*)PyDict_GetItemString(annotations, param.name);
        param.kind = posonly_left-- > 0 ? POSITIONAL_ONLY : POSITIONAL_OR_KEYWORD;
        parameters[param_i++] = param;
    }

    if (flags & CO_VARARGS) {
        py_param_t param;
        param.name =
            PyUnicode_AsUTF8AndSize(PyTuple_GetItem(arg_names, pos_count + keyword_only_count), &param.name_len);
        param.value = NULL;
        param.type = (PyTypeObject*)PyDict_GetItemString(annotations, param.name);
        param.kind = VAR_POSITIONAL;
        parameters[param_i++] = param;
    }

    // Keyword - only parameters.
    for (Py_ssize_t i = pos_count; i < pos_count + keyword_only_count; ++i) {
        py_param_t param;
        param.name = PyUnicode_AsUTF8AndSize(PyTuple_GetItem(arg_names, i), &param.name_len);
        param.value = PyDict_GetItemString(kwdefaults, param.name);
        param.type = (PyTypeObject*)PyDict_GetItemString(annotations, param.name);
        param.kind = KEYWORD_ONLY;
        parameters[param_i++] = param;
    }

    if (flags & CO_VARKEYWORDS) {
        py_param_t param;
        param.name = PyUnicode_AsUTF8AndSize(
            PyTuple_GetItem(arg_names, pos_count + keyword_only_count + (flags & CO_VARARGS)), &param.name_len);
        param.value = NULL;
        param.type = (PyTypeObject*)PyDict_GetItemString(annotations, param.name);
        param.kind = VAR_KEYWORD;
        parameters[param_i++] = param;
    }

    wrap->u_params = parameters;
    wrap->params_cnt = total_params;
    return 0;
}

static void wrapper(ujrpc_call_t call, ujrpc_callback_tag_t callback_tag) {
    py_wrapper_t* wrap = (py_wrapper_t*)(callback_tag);
    PyObject* args = PyTuple_New(wrap->params_cnt);

    for (size_t i = 0; i < wrap->params_cnt; ++i) {
        PyTypeObject* type = wrap->u_params[i].type;
        py_param_kind_t kind = wrap->u_params[i].kind;
        bool may_have_name = (kind & POSITIONAL_OR_KEYWORD) | (kind & KEYWORD_ONLY) | (kind & VAR_KEYWORD);
        bool may_have_pos = (kind & POSITIONAL_OR_KEYWORD) | (kind & POSITIONAL_ONLY) | (kind & VAR_POSITIONAL);
        bool got_named = false;
        Py_ssize_t name_len = wrap->u_params[i].name_len;
        ujrpc_str_t name = wrap->u_params[i].name;

        if (PyType_IsSubtype(type, &PyBool_Type)) {
            bool res;
            if (may_have_name && name_len > 0)
                got_named = ujrpc_param_named_bool(call, name, name_len, &res);

            if ((may_have_pos && !got_named) && //
                !ujrpc_param_positional_bool(call, i, &res))
                return ujrpc_call_reply_error_invalid_params(call);

            PyTuple_SetItem(args, i, res ? Py_True : Py_False);
        } else if (PyType_IsSubtype(type, &PyLong_Type)) {
            Py_ssize_t res;
            if (may_have_name && name_len > 0)
                got_named = ujrpc_param_named_i64(call, name, name_len, &res);

            if ((may_have_pos && !got_named) && //
                !ujrpc_param_positional_i64(call, i, &res))
                return ujrpc_call_reply_error_invalid_params(call);

            PyTuple_SetItem(args, i, PyLong_FromSsize_t(res));
        } else if (PyType_IsSubtype(type, &PyFloat_Type)) {
            double res;
            if (may_have_name && name_len > 0)
                got_named = ujrpc_param_named_f64(call, name, name_len, &res);

            if ((may_have_pos && !got_named) && //
                !ujrpc_param_positional_f64(call, i, &res))
                return ujrpc_call_reply_error_invalid_params(call);

            PyTuple_SetItem(args, i, PyFloat_FromDouble(res));
        } else if (PyType_IsSubtype(type, &PyBytes_Type)) {
            ujrpc_str_t res;
            size_t len;
            if (may_have_name && name_len > 0)
                got_named = ujrpc_param_named_str(call, name, name_len, &res, &len);

            if ((may_have_pos && !got_named) && //
                !ujrpc_param_positional_str(call, i, &res, &len))
                return ujrpc_call_reply_error_invalid_params(call);

            len = tb64dec((unsigned char const*)res, len, (unsigned char*)res);
            PyTuple_SetItem(args, i, PyBytes_FromStringAndSize(res, len));
        } else if (PyType_IsSubtype(type, &PyUnicode_Type)) {
            ujrpc_str_t res;
            size_t len;
            if (may_have_name && name_len > 0)
                got_named = ujrpc_param_named_str(call, name, name_len, &res, &len);

            if ((may_have_pos && !got_named) && //
                !ujrpc_param_positional_str(call, i, &res, &len))
                return ujrpc_call_reply_error_invalid_params(call);

            PyTuple_SetItem(args, i, PyUnicode_FromStringAndSize(res, len));
        }
    }

    PyObject* response = PyObject_CallObject(wrap->callable, args);
    if (response == NULL)
        return ujrpc_call_reply_error_unknown(call);

    size_t sz = calculate_size_as_str(response);
    char* parsed_response = (char*)(malloc(sz * sizeof(char)));
    size_t len = 0;
    int res = to_string(response, &parsed_response[0], &len);
    ujrpc_call_reply_content(call, &parsed_response[0], len);
}

static PyObject* server_add_procedure(py_server_t* self, PyObject* args) {
    // Take a function object, introspect its arguments,
    // register them inside of a higher-level function,
    // which on every call requests them via `ujrpc_param_named_...`
    // from the `ujrpc_call_t` context, then wraps them into native
    // Python objects and passes to the original function.
    // The result of that function call must then be returned via
    // the `ujrpc_call_send_content` call.
    py_wrapper_t wrap;
    if (!PyArg_ParseTuple(args, "O", &wrap.callable) || !PyCallable_Check(wrap.callable)) {
        PyErr_SetString(PyExc_TypeError, "Need a callable object!");
        return NULL;
    }

    if (prepare_wrapper(wrap.callable, &wrap) != 0)
        return NULL;

    if (self->count_added >= self->wrapper_capacity) {
        self->wrapper_capacity *= 2;
        self->wrappers = (py_wrapper_t*)realloc(self->wrappers, self->wrapper_capacity);
    }

    self->wrappers[self->count_added] = wrap;

    ujrpc_add_procedure(self->server, PyUnicode_AsUTF8(PyObject_GetAttrString(wrap.callable, "__name__")), wrapper,
                        &self->wrappers[self->count_added]);

    ++self->count_added;
    Py_INCREF(wrap.callable);
    return wrap.callable;
}

static PyObject* server_run(py_server_t* self, PyObject* args) {
    Py_ssize_t max_cycles = -1;
    double max_seconds = -1;
    // If none are provided, it is wiser to use the `ujrpc_take_calls`,
    // as it has a more efficient busy-waiting loop implementation.
    if (!PyArg_ParseTuple(args, "|nd", &max_cycles, &max_seconds)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a cycle count and timeout.");
        return NULL;
    }
    if (max_cycles == -1 && max_seconds == -1) {
        ujrpc_take_calls(self->server, 0);
    } else if (max_cycles == -1) {
        time_t start, end;
        time(&start);
        while (max_seconds > 0) {
            ujrpc_take_call(self->server, 0);
            time(&end);
            max_seconds -= difftime(end, start);
            start = end;
        }
    } else if (max_seconds == -1) {
        while (max_cycles > 0) {
            ujrpc_take_call(self->server, 0);
            --max_cycles;
        }
    } else {
        time_t start, end;
        time(&start);
        while (max_cycles > 0 && max_seconds > 0) {
            ujrpc_take_call(self->server, self->count_threads);
            --max_cycles;
            time(&end);
            max_seconds -= difftime(end, start);
            start = end;
        }
    }
    return Py_None;
}

static Py_ssize_t server_callbacks_count(py_server_t* self, PyObject* _) { return self->count_added; }
static PyObject* server_port(py_server_t* self, PyObject* _) { return PyLong_FromLong(self->config.port); }
static PyObject* server_queue_depth(py_server_t* self, PyObject* _) {
    return PyLong_FromLong(self->config.queue_depth);
}
static PyObject* server_max_lifetime(py_server_t* self, PyObject* _) {
    return PyLong_FromLong(self->config.max_lifetime_micro_seconds);
}

static PyMethodDef server_methods[] = {
    {"route", (PyCFunction)&server_add_procedure, METH_VARARGS, PyDoc_STR("Append a procedure callback")},
    {"run", (PyCFunction)&server_run, METH_VARARGS,
     PyDoc_STR("Runs the server for N calls or T seconds, before returning")},
    {NULL},
};

static PyGetSetDef server_computed_properties[] = {
    {"port", (getter)&server_port, NULL, PyDoc_STR("On which port the server listens")},
    {"queue_depth", (getter)&server_queue_depth, NULL, PyDoc_STR("Max number of concurrent users")},
    {"max_lifetime", (getter)&server_max_lifetime, NULL, PyDoc_STR("Max lifetime of connections in microseconds")},
    {NULL},
};

static PyMappingMethods server_mapping_methods = {
    .mp_length = (lenfunc)server_callbacks_count,
    .mp_subscript = NULL,
    .mp_ass_subscript = NULL,
};

static void server_dealloc(py_server_t* self) {
    free(self->wrappers);
    ujrpc_free(self->server);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* server_new(PyTypeObject* type, PyObject* args, PyObject* keywords) {
    py_server_t* self = (py_server_t*)type->tp_alloc(type, 0);
    return (PyObject*)self;
}

static int server_init(py_server_t* self, PyObject* args, PyObject* keywords) {
    static const char const* keywords_list[7] = {
        "interface", "port", "queue_depth", "max_callbacks", "max_threads", "count_threads", NULL,
    };
    self->config.interface = "0.0.0.0";
    self->config.port = 8545;
    self->config.queue_depth = 4096;
    self->config.max_callbacks = UINT16_MAX;
    self->config.max_threads = 16;
    self->config.max_concurrent_connections = 1024;
    self->config.max_lifetime_exchanges = UINT32_MAX;
    self->count_threads = 1;

    if (!PyArg_ParseTupleAndKeywords(args, keywords, "|snnnnn", (char**)keywords_list, //
                                     &self->config.interface, &self->config.port, &self->config.queue_depth,
                                     &self->config.max_callbacks, &self->config.max_threads, &self->count_threads))
        return -1;

    self->wrapper_capacity = 16;
    self->wrappers = (py_wrapper_t*)malloc(self->wrapper_capacity * sizeof(py_wrapper_t));

    // Initialize the server
    ujrpc_init(&self->config, &self->server);
    return 0;
}

// Order: https://docs.python.org/3/c-api/typeobj.html#quick-reference
static PyTypeObject ujrpc_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "ujrpc." stringify_value_m(UKV_PYTHON_MODULE_NAME) ".Server",
    .tp_basicsize = sizeof(py_server_t),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)server_dealloc,
    // TODO:
    // .tp_vectorcall_offset = 0,
    // .tp_repr = NULL,
    .tp_as_mapping = &server_mapping_methods,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc = PyDoc_STR("Server class for Remote Procedure Calls implemented in Python"),
    .tp_methods = server_methods,
    .tp_call = (ternaryfunc)&server_add_procedure,
    .tp_getset = server_computed_properties,
    .tp_init = (initproc)server_init,
    .tp_new = server_new,
};

static PyModuleDef server_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "ujrpc." stringify_value_m(UKV_PYTHON_MODULE_NAME),
    .m_doc = "Uninterrupted JSON Remote Procedure Calls library.",
    .m_size = -1,
};

PyMODINIT_FUNC pyinit_f_m(void) {
    if (PyType_Ready(&ujrpc_type) < 0)
        return NULL;

    PyObject* m = PyModule_Create(&server_module);
    if (!m)
        return NULL;

    Py_INCREF(&ujrpc_type);
    if (PyModule_AddObject(m, "Server", (PyObject*)&ujrpc_type) < 0) {
        Py_DECREF(&ujrpc_type);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}

int main(int argc, char* argv[]) {
    wchar_t* program = Py_DecodeLocale(argv[0], NULL);
    if (!program) {
        fprintf(stderr, "Fatal error: cannot decode argv[0]\n");
        exit(1);
    }

    /* Add a built-in module, before Py_Initialize */
    if (PyImport_AppendInittab("ujrpc." stringify_value_m(UKV_PYTHON_MODULE_NAME), pyinit_f_m) == -1) {
        fprintf(stderr, "Error: could not extend in-built modules table\n");
        exit(1);
    }

    /* Pass argv[0] to the Python interpreter */
    Py_SetProgramName(program);

    /* Initialize the Python interpreter.  Required.
       If this step fails, it will be a fatal error. */
    Py_Initialize();

    /* Optionally import the module; alternatively,
       import can be deferred until the embedded script
       imports it. */
    PyObject* pmodule = PyImport_ImportModule("ujrpc." stringify_value_m(UKV_PYTHON_MODULE_NAME));
    if (!pmodule) {
        PyErr_Print();
        fprintf(stderr, "Error: could not import module 'ujrpc'\n");
    }
    PyMem_RawFree(program);
    return 0;
}
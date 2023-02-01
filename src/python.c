/**
 * @file python.cpp
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
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "ujrpc/ujrpc.h"

typedef struct {
    PyObject_HEAD;
    ujrpc_config_t config;
    ujrpc_server_t server;
    size_t count_added;
} py_server_t;

static PyObject* server_add_procedure(py_server_t* self, PyObject* args) {
    // Take a function object, introspect its arguments,
    // register them inside of a higher-level function,
    // which on every call requests them via `ujrpc_param_named_...`
    // from the `ujrpc_call_t` context, then wraps them into native
    // Python objects and passes to the original function.
    // The result of that function call must then be returned via
    // the `ujrpc_call_send_content` call.
}

static PyObject* server_run(py_server_t* self, PyObject* args) {
    Py_ssize_t max_cycles;
    Py_ssize_t max_seconds;
    if (!PyArg_ParseTuple(args, "nn", &max_seconds, &max_cycles)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a optional....");
        return NULL;
    }

    ujrpc_take_call(self->server);
}

static Py_ssize_t server_callbacks_count(py_server_t* self, PyObject* _) { return self->count_added; }
static PyObject* server_port(py_server_t* self, PyObject* _) { return self->config.port; }
static PyObject* server_max_connections(py_server_t* self, PyObject* _) { return 0; }
static PyObject* server_max_lifetime(py_server_t* self, PyObject* _) { return 0; }

static PyMethodDef server_methods[] = {
    {"add", (PyCFunction)&server_add_procedure, METH_VARARGS, PyDoc_STR("Append a procedure callback")},
    {"run", (PyCFunction)&server_run, METH_VARARGS,
     PyDoc_STR("Runs the server for N calls or T seconds, before returning")},
    {NULL},
};

static PyGetSetDef server_computed_properties[] = {
    {"port", (getter)&server_port, NULL, PyDoc_STR("On which port the server listens")},
    {"max_connections", (getter)&server_max_connections, NULL, PyDoc_STR("Max number of concurrent users")},
    {"max_lifetime", (getter)&server_max_lifetime, NULL, PyDoc_STR("Max lifetime of connections in microseconds")},
    {NULL},
};

static PyMappingMethods server_mapping_methods = {
    .mp_length = (lenfunc)server_callbacks_count,
    .mp_subscript = NULL,
    .mp_ass_subscript = NULL,
};

static void server_dealloc(py_server_t* self) {
    ujrpc_free(self->server);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* server_new(PyTypeObject* type, PyObject* args, PyObject* keywords) {
    py_server_t* self = (py_server_t*)type->tp_alloc(type, 0);
    return (PyObject*)self;
}

static int server_init(py_server_t* self, PyObject* args, PyObject* keywords) {

    static char const* keywords_list[] = {"port", "max_connections", "max_lifetime", NULL};
    Py_ssize_t port = 0, max_connections = 0, max_lifetime = UINT64_MAX;
    char const* dtype = NULL;
    char const* metric = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, keywords, "nn|nss", (char**)keywords_list, //
                                     &port, &max_connections, &max_lifetime, &dtype, &metric))
        return -1;

    ujrpc_config_t params;
    params.port = (uint16_t)(port);
    // params.connections_capacity = (uint16_t)(max_connections);
    // params.lifetime_microsec_limit = (uint16_t)(max_lifetime);

    // Initialize the server
    ujrpc_init(&self->config, &self->server);
}

// Order: https://docs.python.org/3/c-api/typeobj.html#quick-reference
static PyTypeObject ujrpc_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "ujrpc.Server",
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
    .tp_getset = server_computed_properties,
    .tp_init = (initproc)server_init,
    .tp_new = server_new,
};

static PyModuleDef server_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "ujrpc",
    .m_doc = "Uninterrupted JSON Remote Procedure Calls library.",
    .m_size = -1,
};

PyMODINIT_FUNC PyInit_ujrpc(void) {
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
    if (PyImport_AppendInittab("ujrpc", PyInit_ujrpc) == -1) {
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
    PyObject* pmodule = PyImport_ImportModule("ujrpc");
    if (!pmodule) {
        PyErr_Print();
        fprintf(stderr, "Error: could not import module 'ujrpc'\n");
    }
    PyMem_RawFree(program);
    return 0;
}
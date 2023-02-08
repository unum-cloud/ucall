#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <Python.h>

static const char int_to_hex_k[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

static void char_to_hex(uint8_t const c, uint8_t* hex) {
    hex[0] = int_to_hex_k[c >> 4];
    hex[1] = int_to_hex_k[c & 0x0F];
}

static int to_string(PyObject* obj, char* data, size_t* len) {
    if (obj == Py_None)
        *len = sprintf(data, "%s", "null");
    else if (PyBool_Check(obj))
        *len = sprintf(data, "%s", obj == Py_False ? "false" : "true");
    else if (PyLong_Check(obj))
        *len = sprintf(data, "%li", PyLong_AsLong(obj));
    else if (PyFloat_Check(obj))
        *len = sprintf(data, "%f", PyFloat_AsDouble(obj));
    else if (PyBytes_Check(obj))
        *len = sprintf(data, "\"%s\"", PyBytes_AsString(obj));
    else if (PyUnicode_Check(obj)) {
        Py_ssize_t size;
        const char* char_ptr = PyUnicode_AsUTF8AndSize(obj, &size);
        char unc[size + 2];
        unc[0] = '"';
        size_t pos = 1;
        for (size_t i = 0; i != size; ++i) {
            uint8_t c = char_ptr[i];
            switch (c) {
            case 34:
                pos += sprintf(&unc[pos], "\\\"");
                break;
            case 92:
                pos += sprintf(&unc[pos], "\\\\");
                break;
            case 8:
                pos += sprintf(&unc[pos], "\\b");
                break;
            case 9:
                pos += sprintf(&unc[pos], "\\t");
                break;
            case 10:
                pos += sprintf(&unc[pos], "\\n");
                break;
            case 12:
                pos += sprintf(&unc[pos], "\\f");
                break;
            case 13:
                pos += sprintf(&unc[pos], "\\r");
                break;
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
            case 11:
            case 14:
            case 15:
            case 16:
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
            case 22:
            case 23:
            case 24:
            case 25:
            case 26:
            case 27:
            case 28:
            case 29:
            case 30:
            case 31: {
                pos += sprintf(&unc[pos], "\\u0000");
                char_to_hex(c, &unc[pos - 2]);
                break;
            }
            default:
                unc[pos++] = char_ptr[i];
            }
        }
        unc[pos] = '"';
        memmove(data, &unc[0], size + 2);
        *len = size + 2;
    } else if (PySequence_Check(obj)) {
        char* begin = data;
        *len = 0;
        begin[(*len)++] = '[';
        if (!PySequence_Length(obj)) {
            begin[(*len)++] = ']';
        } else {
            for (Py_ssize_t i = 0; i < PySequence_Length(obj); i++) {
                size_t n_len = 0;
                to_string(PySequence_GetItem(obj, i), &begin[*len], &n_len);
                *len += n_len;
                begin[(*len)++] = ',';
            }
            begin[*len - 1] = ']';
        }
    } else if (PyDict_Check(obj)) {
        char* begin = data;
        *len = 0;
        begin[(*len)++] = '{';
        if (!PyDict_Size(obj)) {
            begin[(*len)++] = '}';
        } else {
            PyObject *key, *value;
            Py_ssize_t pos = 0;
            while (PyDict_Next(obj, &pos, &key, &value)) {
                size_t n_len = 0;
                to_string(key, &begin[*len], &n_len);
                *len += n_len;
                begin[(*len)++] = ':';
                n_len = 0;
                to_string(value, &begin[*len], &n_len);
                *len += n_len;
                begin[(*len)++] = ',';
            }
            begin[*len - 1] = '}';
        }
    } else
        return -1;
    return 0;
}

#ifdef __cplusplus
} /* end extern "C" */
#endif
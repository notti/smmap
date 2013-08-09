/* this is a mix of _struct.c and mmapmodule.c from Python 2.7 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include <string.h>

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */

#define FLOAT_COERCE_WARN "integer argument expected, got float"
#define NON_INTEGER_WARN "integer argument expected, got non-integer " \
    "(implicit conversion using __int__ is deprecated)"

#define STRINGIFY(x)    #x

static PyObject *mmap_module_error;

typedef enum
{
    ACCESS_DEFAULT,
    ACCESS_READ,
    ACCESS_WRITE,
} access_mode;

typedef struct _formatdef {
    char format;
    Py_ssize_t size;
    PyObject* (*get)(const void *, Py_ssize_t);
    int (*set)(void *, PyObject*, Py_ssize_t);
} formatdef;

typedef struct {
    PyObject_HEAD
    void *      data;
    size_t      size;
    Py_ssize_t  elem;
    off_t       offset;
    char        type;

    access_mode access;

    PyObject* (*get)(const void *, Py_ssize_t);
    int (*set)(void *, PyObject*, Py_ssize_t);
} mmap_object;

static PyObject *
get_pylong(PyObject *v)
{
    PyObject *r, *w;
    int converted = 0;
    assert(v != NULL);
    if (!PyInt_Check(v) && !PyLong_Check(v)) {
        PyNumberMethods *m;
        /* Not an integer; first try to use __index__ to
           convert to an integer.  If the __index__ method
           doesn't exist, or raises a TypeError, try __int__.
           Use of the latter is deprecated, and will fail in
           Python 3.x. */

        m = Py_TYPE(v)->tp_as_number;
        if (PyIndex_Check(v)) {
            w = PyNumber_Index(v);
            if (w != NULL) {
                v = w;
                /* successfully converted to an integer */
                converted = 1;
            }
            else if (PyErr_ExceptionMatches(PyExc_TypeError)) {
                PyErr_Clear();
            }
            else
                return NULL;
        }
        if (!converted && m != NULL && m->nb_int != NULL) {
            /* Special case warning message for floats, for
               backwards compatibility. */
            if (PyFloat_Check(v)) {
                if (PyErr_WarnEx(
                            PyExc_DeprecationWarning,
                            FLOAT_COERCE_WARN, 1))
                    return NULL;
            }
            else {
                if (PyErr_WarnEx(
                            PyExc_DeprecationWarning,
                            NON_INTEGER_WARN, 1))
                    return NULL;
            }
            v = m->nb_int(v);
            if (v == NULL)
                return NULL;
            if (!PyInt_Check(v) && !PyLong_Check(v)) {
                PyErr_SetString(PyExc_TypeError,
                                "__int__ method returned "
                                "non-integer");
                return NULL;
            }
            converted = 1;
        }
        if (!converted) {
            PyErr_SetString(PyExc_ValueError,
                            "cannot convert argument "
                            "to integer");
            return NULL;
        }
    }
    else
        /* Ensure we own a reference to v. */
        Py_INCREF(v);

    assert(PyInt_Check(v) || PyLong_Check(v));
    if (PyInt_Check(v)) {
        r = PyLong_FromLong(PyInt_AS_LONG(v));
        Py_DECREF(v);
    }
    else if (PyLong_Check(v)) {
        assert(PyLong_Check(v));
        r = v;
    }
    else {
        r = NULL;   /* silence compiler warning about
                       possibly uninitialized variable */
        assert(0);  /* shouldn't ever get here */
    }

    return r;
}

/* Helper to convert a Python object to a C long.  Sets an exception
   (struct.error for an inconvertible type, OverflowError for
   out-of-range values) and returns -1 on error. */

static int
get_long(PyObject *v, long *p)
{
    long x;

    v = get_pylong(v);
    if (v == NULL)
        return -1;
    assert(PyLong_Check(v));
    x = PyLong_AsLong(v);
    Py_DECREF(v);
    if (x == (long)-1 && PyErr_Occurred())
        return -1;
    *p = x;
    return 0;
}

/* Same, but handling unsigned long */

static int
get_ulong(PyObject *v, unsigned long *p)
{
    unsigned long x;

    v = get_pylong(v);
    if (v == NULL)
        return -1;
    assert(PyLong_Check(v));
    x = PyLong_AsUnsignedLong(v);
    Py_DECREF(v);
    if (x == (unsigned long)-1 && PyErr_Occurred())
        return -1;
    *p = x;
    return 0;
}

static PyObject *
nu_byte(const void *p, Py_ssize_t i)
{
    return PyInt_FromLong((long) ((signed char *)p)[i]);
}

static PyObject *
nu_ubyte(const void *p, Py_ssize_t i)
{
    return PyInt_FromLong((long) ((unsigned char *)p)[i]);
}

static PyObject *
nu_short(const void *p, Py_ssize_t i)
{
    return PyInt_FromLong((long) ((short *)p)[i]);
}

static PyObject *
nu_ushort(const void *p, Py_ssize_t i)
{
    return PyInt_FromLong((long) ((unsigned short *)p)[i]);
}

static PyObject *
nu_int(const void *p, Py_ssize_t i)
{
    return PyInt_FromLong((long) ((int *)p)[i]);
}

static PyObject *
nu_uint(const void *p, Py_ssize_t i)
{
    unsigned int x = ((unsigned int *)p)[i];
#if (SIZEOF_LONG > SIZEOF_INT)
    return PyInt_FromLong((long) x);
#else
    if (x <= ((unsigned int)LONG_MAX))
        return PyInt_FromLong((long) x);
    return PyLong_FromUnsignedLong((unsigned long) x);
#endif
}

static PyObject *
nu_long(const void *p, Py_ssize_t i)
{
    return PyInt_FromLong(((long *)p)[i]);
}

static PyObject *
nu_ulong(const void *p, Py_ssize_t i)
{
    unsigned long x = ((long *)p)[i];
    if (x <= LONG_MAX)
        return PyInt_FromLong((long)x);
    return PyLong_FromUnsignedLong(x);
}

static PyObject *
nu_float(const void *p, Py_ssize_t i)
{
    return PyFloat_FromDouble((double) ((float *)p)[i]);
}

static PyObject *
nu_double(const void *p, Py_ssize_t i)
{
    return PyFloat_FromDouble(((double *)p)[i]);
}

static int
np_byte(void *p, PyObject *v, Py_ssize_t i)
{
    long x;
    if (get_long(v, &x) < 0)
        return -1;
    if (x < -128 || x > 127){
        PyErr_SetString(PyExc_ValueError,
                        "byte format requires -128 <= number <= 127");
        return -1;
    }
    ((char *)p)[i] = (char)x;
    return 0;
}

static int
np_ubyte(void *p, PyObject *v, Py_ssize_t i)
{
    long x;
    if (get_long(v, &x) < 0)
        return -1;
    if (x < 0 || x > 255){
        PyErr_SetString(PyExc_ValueError,
                        "ubyte format requires 0 <= number <= 255");
        return -1;
    }
    ((char *)p)[i] = (char)x;
    return 0;
}

static int
np_short(void *p, PyObject *v, Py_ssize_t i)
{
    long x;
    if (get_long(v, &x) < 0)
        return -1;
    if (x < SHRT_MIN || x > SHRT_MAX){
        PyErr_SetString(PyExc_ValueError,
                        "short format requires " STRINGIFY(SHRT_MIN)
                        " <= number <= " STRINGIFY(SHRT_MAX));
        return -1;
    }
    ((short *)p)[i] = (short)x;
    return 0;
}

static int
np_ushort(void *p, PyObject *v, Py_ssize_t i)
{
    long x;
    if (get_long(v, &x) < 0)
        return -1;
    if (x < 0 || x > USHRT_MAX){
        PyErr_SetString(PyExc_ValueError,
                        "ushort format requires 0 <= number <= " STRINGIFY(USHRT_MAX));
        return -1;
    }
    ((unsigned short *)p)[i] = (unsigned short)x;
    return 0;
}

static int
np_int(void *p, PyObject *v, Py_ssize_t i)
{
    long x;
    if (get_long(v, &x) < 0)
        return -1;
#if (SIZEOF_LONG > SIZEOF_INT)
    if ((x < ((long)INT_MIN)) || (x > ((long)INT_MAX))){
        PyErr_SetString(PyExc_ValueError,
                        "int format requires " STRINGIFY(INT_MIN)
                        " <= number <= " STRINGIFY(INT_MAX));
        return -1;
    }
#endif
    ((int *)p)[i] = (int)x;
    return 0;
}

static int
np_uint(void *p, PyObject *v, Py_ssize_t i)
{
    unsigned long x;
    if (get_ulong(v, &x) < 0)
        return -1;
#if (SIZEOF_LONG > SIZEOF_INT)
    if (x > ((unsigned long)UINT_MAX)){
        PyErr_SetString(PyExc_ValueError,
                        "uint format requires 0 <= number <= " STRINGIFY(UINT_MAX));
        return -1;
    }
#endif
    ((unsigned int *)p)[i] = (unsigned int)x;
    return 0;
}

static int
np_long(void *p, PyObject *v, Py_ssize_t i)
{
    long x;
    if (get_long(v, &x) < 0)
        return -1;
    ((long *)p)[i] = x;
    return 0;
}

static int
np_ulong(void *p, PyObject *v, Py_ssize_t i)
{
    unsigned long x;
    if (get_ulong(v, &x) < 0)
        return -1;
    ((unsigned long *)p)[i] = x;
    return 0;
}

static int
np_float(void *p, PyObject *v, Py_ssize_t i)
{
    float x = (float)PyFloat_AsDouble(v);
    if (x == -1 && PyErr_Occurred()) {
        PyErr_SetString(PyExc_ValueError,
                        "required argument is not a float");
        return -1;
    }
    ((float *)p)[i] = x;
    return 0;
}

static int
np_double(void *p, PyObject *v, Py_ssize_t i)
{
    double x = PyFloat_AsDouble(v);
    if (x == -1 && PyErr_Occurred()) {
        PyErr_SetString(PyExc_ValueError,
                        "required argument is not a float");
        return -1;
    }
    ((double *)p)[i] = x;
    return 0;
}

static formatdef format_table[] = {
    {'b',       sizeof(char),   nu_byte,        np_byte},
    {'B',       sizeof(char),   nu_ubyte,       np_ubyte},
    {'h',       sizeof(short),  nu_short,       np_short},
    {'H',       sizeof(short),  nu_ushort,      np_ushort},
    {'i',       sizeof(int),    nu_int,         np_int},
    {'I',       sizeof(int),    nu_uint,        np_uint},
    {'l',       sizeof(long),   nu_long,        np_long},
    {'L',       sizeof(long),   nu_ulong,       np_ulong},
    {'f',       sizeof(float),  nu_float,       np_float},
    {'d',       sizeof(double), nu_double,      np_double},
    {0}
};

static void
mmap_object_dealloc(mmap_object *m_obj)
{
    if (m_obj->data!=NULL) {
        munmap(m_obj->data, m_obj->size);
    }

    Py_TYPE(m_obj)->tp_free((PyObject*)m_obj);
}

static PyObject *
mmap_close_method(mmap_object *self, PyObject *unused)
{
    if (self->data != NULL) {
        munmap(self->data, self->size);
        self->data = NULL;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

#define CHECK_VALID(err)                                                \
do {                                                                    \
    if (self->data == NULL) {                                           \
    PyErr_SetString(PyExc_ValueError, "smmap closed or invalid");       \
    return err;                                                         \
    }                                                                   \
} while (0)

static int
is_writeable(mmap_object *self)
{
    if (self->access != ACCESS_READ)
        return 1;
    PyErr_Format(PyExc_TypeError, "smmap can't modify a readonly memory map.");
    return 0;
}

static struct PyMethodDef mmap_object_methods[] = {
    {"close",           (PyCFunction) mmap_close_method,        METH_NOARGS},
    {NULL,         NULL}       /* sentinel */
};

/* Functions for treating an mmap'ed file as a buffer */

static Py_ssize_t
mmap_buffer_getreadbuf(mmap_object *self, Py_ssize_t index, const void **ptr)
{
    CHECK_VALID(-1);
    if (index != 0) {
        PyErr_SetString(PyExc_SystemError,
                        "Accessing non-existent smmap segment");
        return -1;
    }
    *ptr = self->data;
    return self->size;
}

static Py_ssize_t
mmap_buffer_getwritebuf(mmap_object *self, Py_ssize_t index, const void **ptr)
{
    CHECK_VALID(-1);
    if (index != 0) {
        PyErr_SetString(PyExc_SystemError,
                        "Accessing non-existent smmap segment");
        return -1;
    }
    if (!is_writeable(self))
        return -1;
    *ptr = self->data;
    return self->size;
}

static Py_ssize_t
mmap_buffer_getsegcount(mmap_object *self, Py_ssize_t *lenp)
{
    CHECK_VALID(-1);
    if (lenp)
        *lenp = self->size;
    return 1;
}

static Py_ssize_t
mmap_buffer_getcharbuffer(mmap_object *self, Py_ssize_t index, const void **ptr)
{
    if (index != 0) {
        PyErr_SetString(PyExc_SystemError,
                        "accessing non-existent buffer segment");
        return -1;
    }
    *ptr = (const char *)self->data;
    return self->size;
}

static Py_ssize_t
mmap_length(mmap_object *self)
{
    CHECK_VALID(-1);
    return self->elem;
}

static PyObject *
mmap_item(mmap_object *self, Py_ssize_t i)
{
    CHECK_VALID(NULL);
    if (i < 0 || i >= self->elem) {
        PyErr_SetString(PyExc_IndexError, "smmap index out of range");
        return NULL;
    }
    return self->get(self->data, i);
}

static PyObject *
mmap_slice(mmap_object *self, Py_ssize_t ilow, Py_ssize_t ihigh)
{
    PyObject *ret;
    PyObject *item;
    Py_ssize_t len;
    Py_ssize_t i;

    CHECK_VALID(NULL);
    if (ilow < 0)
        ilow = 0;
    else if (ilow > self->elem)
        ilow = self->elem;
    if (ihigh < 0)
        ihigh = 0;
    if (ihigh < ilow)
        ihigh = ilow;
    else if (ihigh > self->elem)
        ihigh = self->elem;

    len = ihigh - ilow;

    if ((ret = PyTuple_New(len)) == NULL) {
        return NULL;
    }

    for (i = 0; i < len; i++) {
        item = self->get(self->data, i + ilow);
        if (!item) {
            Py_DECREF(ret);
            return NULL;
        }
        PyTuple_SET_ITEM(ret, i, item);
    }

    return ret;
}

static PyObject *
mmap_concat(mmap_object *self, PyObject *bb)
{
    CHECK_VALID(NULL);
    PyErr_SetString(PyExc_SystemError,
                    "smmaps don't support concatenation");
    return NULL;
}

static PyObject *
mmap_repeat(mmap_object *self, Py_ssize_t n)
{
    CHECK_VALID(NULL);
    PyErr_SetString(PyExc_SystemError,
                    "smmaps don't support repeat operation");
    return NULL;
}

static int
mmap_ass_slice(mmap_object *self, Py_ssize_t ilow, Py_ssize_t ihigh, PyObject *v)
{
    PyObject *seq;
    PyObject **items;
    Py_ssize_t i;
    Py_ssize_t len;

    CHECK_VALID(-1);
    if (ilow < 0)
        ilow = 0;
    else if (ilow > self->elem)
        ilow = self->elem;
    if (ihigh < 0)
        ihigh = 0;
    if (ihigh < ilow)
        ihigh = ilow;
    else if (ihigh > self->elem)
        ihigh = self->elem;

    len = ihigh - ilow;

    if (v == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "smmap object doesn't support slice deletion");
        return -1;
    }
    if ((seq = PySequence_Fast(v, "smmap slice assignment must be a sequence")) == NULL ) {
        return -1;
    }
    if (PySequence_Fast_GET_SIZE(seq) != len) {
        PyErr_SetString(PyExc_IndexError,
                        "smmap slice assignment is wrong size");
        Py_DECREF(seq);
        return -1;
    }
    if (!is_writeable(self)) {
        Py_DECREF(seq);
        return -1;
    }
    items = PySequence_Fast_ITEMS(seq);
    for(i = 0; i < len; i++) {
        if (self->set(self->data, items[i], i + ilow) == -1) {
            Py_DECREF(seq);
            return -1;
        }
    }
    Py_DECREF(seq);
    return 0;
}

static int
mmap_ass_item(mmap_object *self, Py_ssize_t i, PyObject *v)
{
    CHECK_VALID(-1);
    if (i < 0 || i >= self->elem) {
        PyErr_SetString(PyExc_IndexError, "smmap index out of range");
        return -1;
    }
    if (v == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "smmap object doesn't support item deletion");
        return -1;
    }
    if (!is_writeable(self))
        return -1;
    return self->set(self->data, v, i);
}

static PySequenceMethods mmap_as_sequence = {
    (lenfunc)mmap_length,                      /*sq_length*/
    (binaryfunc)mmap_concat,                   /*sq_concat*/
    (ssizeargfunc)mmap_repeat,                 /*sq_repeat*/
    (ssizeargfunc)mmap_item,                           /*sq_item*/
    (ssizessizeargfunc)mmap_slice,             /*sq_slice*/
    (ssizeobjargproc)mmap_ass_item,            /*sq_ass_item*/
    (ssizessizeobjargproc)mmap_ass_slice,      /*sq_ass_slice*/
};

static PyBufferProcs mmap_as_buffer = {
    (readbufferproc)mmap_buffer_getreadbuf,
    (writebufferproc)mmap_buffer_getwritebuf,
    (segcountproc)mmap_buffer_getsegcount,
    (charbufferproc)mmap_buffer_getcharbuffer,
};

static PyObject *
new_mmap_object(PyTypeObject *type, PyObject *args, PyObject *kwdict);

PyDoc_STRVAR(mmap_doc,
"mmap(fileno, length, format[, access[, offset]])\n\
\n\
Maps length bytes from the file specified by the file descriptor fileno,\n\
and returns a mmap object.\n\
Format specifies the number format like in the struct module.\n\
b signed char\n\
B unsigned char\n\
h short\n\
H unsigned short\n\
i int\n\
I unsigned int\n\
l long\n\
L unsigned long\n\
f float\n\
d double");


static PyTypeObject mmap_object_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "smmap.mmap",                               /* tp_name */
    sizeof(mmap_object),                        /* tp_size */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor) mmap_object_dealloc,           /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    &mmap_as_sequence,                          /*tp_as_sequence*/
    0,                                          /*tp_as_mapping*/
    0,                                          /*tp_hash*/
    0,                                          /*tp_call*/
    0,                                          /*tp_str*/
    PyObject_GenericGetAttr,                    /*tp_getattro*/
    0,                                          /*tp_setattro*/
    &mmap_as_buffer,                            /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GETCHARBUFFER,                   /*tp_flags*/
    mmap_doc,                                   /*tp_doc*/
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    mmap_object_methods,                        /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                      /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    new_mmap_object,                            /* tp_new */
    PyObject_Del,                           /* tp_free */
};


/* extract the map size from the given PyObject

   Returns -1 on error, with an appropriate Python exception raised. On
   success, the map size is returned. */
static Py_ssize_t
_GetMapSize(PyObject *o, const char* param)
{
    if (o == NULL)
        return 0;
    if (PyIndex_Check(o)) {
        Py_ssize_t i = PyNumber_AsSsize_t(o, PyExc_OverflowError);
        if (i==-1 && PyErr_Occurred())
            return -1;
        if (i < 0) {
            PyErr_Format(PyExc_OverflowError,
                            "memory mapped %s must be positive",
                            param);
            return -1;
        }
        return i;
    }

    PyErr_SetString(PyExc_TypeError, "map size must be an integral value");
    return -1;
}

static const formatdef *
getentry(int c, const formatdef *f)
{
    for (; f->format != '\0'; f++) {
        if (f->format == c) {
            return f;
        }
    }
    return NULL;
}

#ifdef HAVE_LARGEFILE_SUPPORT
#define _Py_PARSE_OFF_T "L"
#else
#define _Py_PARSE_OFF_T "l"
#endif

static PyObject *
new_mmap_object(PyTypeObject *type, PyObject *args, PyObject *kwdict)
{
    mmap_object *m_obj;
    PyObject *map_size_obj = NULL;
    Py_ssize_t map_size;
    off_t offset = 0;
    int fd, prot = PROT_WRITE | PROT_READ;
    int access = (int)ACCESS_DEFAULT;
    char *fmt = " ";
    const formatdef *format;
    static char *keywords[] = {"fileno", "length", "format",
                                     "access", "offset", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwdict, "iOs|iii" _Py_PARSE_OFF_T, keywords,
                                     &fd, &map_size_obj, &fmt, &prot,
                                     &access, &offset))
        return NULL;
    map_size = _GetMapSize(map_size_obj, "size");
    if (map_size < 0)
        return NULL;
    if (offset < 0) {
        PyErr_SetString(PyExc_OverflowError,
            "memory mapped offset must be positive");
        return NULL;
    }

    if (fmt == NULL || *fmt == 0 || (format = getentry(*fmt, format_table)) == NULL) {
        PyErr_SetString(PyExc_ValueError, "bad char in struct format");
        return NULL;
    }

    switch ((access_mode)access) {
    case ACCESS_READ:
        prot = PROT_READ;
        break;
    case ACCESS_WRITE:
        prot = PROT_READ | PROT_WRITE;
        break;
    case ACCESS_DEFAULT:
        break;
    default:
        return PyErr_Format(PyExc_ValueError,
                            "smmap invalid access parameter.");
    }

    m_obj = (mmap_object *)type->tp_alloc(type, 0);
    if (m_obj == NULL) {return NULL;}
    m_obj->data = NULL;
    m_obj->size = (size_t) (map_size * format->size);
    m_obj->offset = offset;
    m_obj->data = mmap(NULL, map_size,
                       prot, MAP_SHARED,
                       fd, offset);
    m_obj->get = format->get;
    m_obj->set = format->set;
    m_obj->elem = map_size;

    if (m_obj->data == (void *)-1) {
        m_obj->data = NULL;
        Py_DECREF(m_obj);
        PyErr_SetFromErrno(mmap_module_error);
        return NULL;
    }
    m_obj->access = (access_mode)access;
    return (PyObject *)m_obj;
}

static void
setint(PyObject *d, const char *name, long value)
{
    PyObject *o = PyInt_FromLong(value);
    if (o && PyDict_SetItemString(d, name, o) == 0) {
        Py_DECREF(o);
    }
}

PyMODINIT_FUNC
initsmmap(void)
{
    PyObject *dict, *module;

    if (PyType_Ready(&mmap_object_type) < 0)
        return;

    module = Py_InitModule("smmap", NULL);
    if (module == NULL)
        return;
    dict = PyModule_GetDict(module);
    if (!dict)
        return;
    mmap_module_error = PyErr_NewException("smmap.error",
        PyExc_EnvironmentError , NULL);
    if (mmap_module_error == NULL)
        return;
    PyDict_SetItemString(dict, "error", mmap_module_error);
    PyDict_SetItemString(dict, "mmap", (PyObject*) &mmap_object_type);

    setint(dict, "ACCESS_READ", ACCESS_READ);
    setint(dict, "ACCESS_WRITE", ACCESS_WRITE);
}


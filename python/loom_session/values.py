# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Joshua DeMoss
"""Zen's compat JSON envelope, encoded and decoded against the HOST'S schemas.

A value crosses as ``{"zen":1,"schema":"<name>","version":N,"fields":{...}}`` -- Loom's own
compat codec (``zen/serialize.hpp``), which the host re-admits through the one gate. The JSON
alone cannot say whether ``"42"`` is an Int or a Text, so this module never guesses: it asks the
host for the shape's descriptor (the bridge's Describe, answered with ``zen.SchemaDesc``) and
reads and writes each field by its declared kind. Structural truth is the host's current schema;
nothing here keeps a second copy of any vocabulary.

Kinds, as the host numbers them: Int 0, Float 1, Text 2, Bool 3, Bytes 4, Message 5, List 6,
Union 7, Any 8. The encoding per kind: Int as a base-10 string, Float as a number (or "NaN" /
"Infinity" / "-Infinity"), Text as a string, Bool as true/false, Bytes as base64, Message as an
object of its fields, List as an array. A Union is an object with one member, its case's name and
value, or a case that carries nothing as its name alone; in Python, ``{"moved": {...}}`` or
``"idle"``. An Any names its shape: ``{"schema": name, "version": n, "fields": {...}}``, both
ways, its fields encoded by the shape the host describes for that name and version. A field a
shape does not declare is refused here, as the gate would, and so is a union given no case,
several, or one it does not declare.
An absent field has one spelling both ways: its key is left out. ``encode`` leaves out a key that
is missing or None, and ``decode`` leaves out an optional field the value did not carry, so a
reader tells absence from a zero by the key (``fields.get(name)``), never by a stand-in value.
"""

import base64
import json
import math

INT, FLOAT, TEXT, BOOL, BYTES, MESSAGE, LIST, UNION, ANY = range(9)
KIND_NAMES = {INT: "Int", FLOAT: "Float", TEXT: "Text", BOOL: "Bool", BYTES: "Bytes",
              MESSAGE: "Message", LIST: "List", UNION: "Union", ANY: "Any"}


class ShapeError(ValueError):
    """A value does not fit the shape it claims -- said here, before anything is sent."""


class TypeRef(object):
    __slots__ = ("kind", "ref", "element")

    def __init__(self, kind, ref=None, element=None):
        self.kind = kind
        self.ref = ref          # (name, version) for a Message or a Union
        self.element = element  # TypeRef for a List

    def label(self):
        if self.kind in (MESSAGE, UNION):
            return "%s v%d" % self.ref
        if self.kind == LIST:
            return "List<%s>" % self.element.label()
        return KIND_NAMES.get(self.kind, "?")


class Field(object):
    __slots__ = ("name", "required", "type", "carries")

    def __init__(self, name, required, type_, carries=True):
        self.name = name
        self.required = required
        self.type = type_
        self.carries = carries  # False only for a union's case that carries nothing


class Schema(object):
    """A shape as the host declares it: its name, version and fields, in order; a union's fields
    are its cases, and its value is exactly one of them."""

    def __init__(self, name, version, fields, is_union=False):
        self.name = name
        self.version = version
        self.fields = fields
        self.is_union = is_union
        self.by_name = dict((f.name, f) for f in fields)

    def describe(self):
        """``[(name, type label, required)]`` -- the structural truth a help view prints; a case
        that carries nothing is labelled ``nothing``."""
        return [(f.name, f.type.label() if f.carries else "nothing", f.required)
                for f in self.fields]


def _type_from_tokens(tokens, i):
    tok = tokens[i]
    kind = int(tok["kind"])
    if kind == LIST:
        element, j = _type_from_tokens(tokens, i + 1)
        return TypeRef(LIST, element=element), j
    if kind in (MESSAGE, UNION):
        return TypeRef(kind, ref=(tok["ref_name"], int(tok["ref_version"]))), i + 1
    return TypeRef(kind), i + 1


def schema_from_desc(fields_json):
    """A Schema from a shape's description: a record's ``zen.SchemaDesc`` or a union's
    ``zen.UnionDesc`` (the bridge's Schema reply), or a ``zen.ShapeDesc`` holding either (the
    entries of a descriptor package)."""
    if set(fields_json) == {"record"} or set(fields_json) == {"union"}:
        (_, desc), = fields_json.items()
        return schema_from_desc(desc)
    if "cases" in fields_json:
        cases = []
        for c in fields_json.get("cases", []):
            carries = bool(c.get("carries", False))
            t = _type_from_tokens(c["type"], 0)[0] if carries else TypeRef(BOOL)
            cases.append(Field(c["name"], True, t, carries))
        return Schema(fields_json["name"], int(fields_json["version"]), cases, is_union=True)
    fields = []
    for f in fields_json.get("fields", []):
        t, _ = _type_from_tokens(f["type"], 0)
        fields.append(Field(f["name"], bool(f.get("required", False)), t))
    return Schema(fields_json["name"], int(fields_json["version"]), fields)


def envelope(name, version, fields_json):
    """The compat envelope's bytes."""
    return json.dumps({"zen": 1, "schema": name, "version": int(version), "fields": fields_json},
                      separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def open_envelope(data):
    """``(name, version, fields_json)`` from envelope bytes; ValueError when they are not one."""
    if isinstance(data, (bytes, bytearray)):
        if data[:1] != b"{":
            raise ValueError("not Zen's compat JSON envelope (this session expects compat "
                             "payloads; did the host admit it to the native encoding?)")
        data = data.decode("utf-8")
    obj = json.loads(data)
    if not isinstance(obj, dict) or obj.get("zen") != 1 or "schema" not in obj:
        raise ValueError("not Zen's compat JSON envelope")
    return obj["schema"], int(obj["version"]), obj.get("fields", {})


_DEFAULTS = {INT: 0, FLOAT: 0.0, TEXT: "", BOOL: False, BYTES: b""}


class Codec(object):
    """Encode Python values to a shape's compat fields and back. ``resolve(name, version)``
    returns the host's Schema for a nested Message field."""

    def __init__(self, resolve):
        self.resolve = resolve

    # ---- Python -> compat JSON ------------------------------------------------------------

    def encode(self, schema, value, path=""):
        if schema.is_union:
            return self._encode_union(schema, value, path)
        if value is None:
            value = {}
        if not isinstance(value, dict):
            raise ShapeError("%s: %s v%d takes an object of fields, not %r"
                             % (path or schema.name, schema.name, schema.version, type(value)))
        out = {}
        for key in value:
            if key not in schema.by_name:
                raise ShapeError("%s%s: %s v%d declares no field '%s' (it has: %s)"
                                 % (path, key, schema.name, schema.version, key,
                                    ", ".join(f.name for f in schema.fields) or "none"))
        for f in schema.fields:
            if f.name not in value or value[f.name] is None:
                continue
            out[f.name] = self._cell(value[f.name], f.type, path + f.name)
        return out

    def _encode_union(self, schema, value, path):
        where = path.rstrip(".") or schema.name
        cases = ", ".join(f.name for f in schema.fields)
        if isinstance(value, str):
            f = schema.by_name.get(value)
            if f is None:
                raise ShapeError("%s: %s v%d has no case '%s' (it has: %s)"
                                 % (where, schema.name, schema.version, value, cases))
            if f.carries:
                raise ShapeError("%s: case '%s' carries a value: {'%s': <value>}"
                                 % (where, value, value))
            return value
        if not isinstance(value, dict) or len(value) != 1:
            raise ShapeError("%s: %s v%d holds exactly one case, as {case: value} or a case's "
                             "name alone, not %r" % (where, schema.name, schema.version, value))
        (name, v), = value.items()
        f = schema.by_name.get(name)
        if f is None:
            raise ShapeError("%s: %s v%d has no case '%s' (it has: %s)"
                             % (where, schema.name, schema.version, name, cases))
        if not f.carries:
            raise ShapeError("%s: case '%s' carries nothing: write its name alone"
                             % (where, name))
        return {name: self._cell(v, f.type, path + name)}

    def _encode_any(self, v, path):
        if not isinstance(v, dict) or set(v) - {"schema", "version", "fields"} or "schema" not in v:
            raise ShapeError("%s: an Any is {'schema': name, 'version': n, 'fields': {...}}, "
                             "not %r" % (path, v))
        name, version = v["schema"], int(v.get("version", 1))
        shape = self.resolve(name, version)
        return {"schema": name, "version": version,
                "fields": self.encode(shape, v.get("fields"), path + ".")}

    def _cell(self, v, t, path):
        k = t.kind
        if k == INT:
            if isinstance(v, bool) or not isinstance(v, int):
                raise ShapeError("%s: an Int, not %r" % (path, v))
            return str(v)
        if k == FLOAT:
            if isinstance(v, bool) or not isinstance(v, (int, float)):
                raise ShapeError("%s: a Float, not %r" % (path, v))
            v = float(v)
            if math.isnan(v):
                return "NaN"
            if math.isinf(v):
                return "Infinity" if v > 0 else "-Infinity"
            return v
        if k == TEXT:
            if not isinstance(v, str):
                raise ShapeError("%s: Text, not %r" % (path, v))
            return v
        if k == BOOL:
            if not isinstance(v, bool):
                raise ShapeError("%s: a Bool, not %r" % (path, v))
            return v
        if k == BYTES:
            if not isinstance(v, (bytes, bytearray)):
                raise ShapeError("%s: Bytes, not %r" % (path, type(v)))
            return base64.b64encode(bytes(v)).decode("ascii")
        if k in (MESSAGE, UNION):
            return self.encode(self.resolve(t.ref[0], t.ref[1]), v, path + ".")
        if k == ANY:
            return self._encode_any(v, path)
        if k == LIST:
            if not isinstance(v, (list, tuple)):
                raise ShapeError("%s: a list, not %r" % (path, type(v)))
            return [self._cell(e, t.element, "%s[%d]" % (path, i)) for i, e in enumerate(v)]
        raise ShapeError("%s: kind %d is not one this client knows" % (path, k))

    # ---- compat JSON -> Python ------------------------------------------------------------

    def decode(self, schema, fields_json):
        if schema.is_union:
            if isinstance(fields_json, str):
                return fields_json
            (name, node), = fields_json.items()
            return {name: self._value(node, schema.by_name[name].type)}
        out = {}
        for f in schema.fields:
            if f.name in fields_json:
                out[f.name] = self._value(fields_json[f.name], f.type)
            elif not f.required:
                continue
            elif f.type.kind in _DEFAULTS:
                out[f.name] = _DEFAULTS[f.type.kind]
            elif f.type.kind == LIST:
                out[f.name] = []
            else:
                out[f.name] = None
        return out

    def _value(self, node, t):
        k = t.kind
        if k == INT:
            return int(node)
        if k == FLOAT:
            if isinstance(node, str):
                return {"NaN": float("nan"), "Infinity": float("inf"),
                        "-Infinity": float("-inf")}[node]
            return float(node)
        if k == TEXT:
            return node
        if k == BOOL:
            return bool(node)
        if k == BYTES:
            return base64.b64decode(node)
        if k in (MESSAGE, UNION):
            if node is None:
                return None
            return self.decode(self.resolve(t.ref[0], t.ref[1]), node)
        if k == ANY:
            name, version = node["schema"], int(node["version"])
            return {"schema": name, "version": version,
                    "fields": self.decode(self.resolve(name, version), node["fields"])}
        if k == LIST:
            return [self._value(e, t.element) for e in node]
        return node

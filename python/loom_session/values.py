# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""Zen's compat JSON envelope, encoded and decoded against the HOST'S schemas.

A value crosses as ``{"zen":1,"schema":"<name>","version":N,"fields":{...}}`` -- Loom's own
compat codec (``zen/serialize.hpp``), which the host re-admits through the one gate. The JSON
alone cannot say whether ``"42"`` is an Int or a Text, so this module never guesses: it asks the
host for the shape's descriptor (the bridge's Describe, answered with ``zen.SchemaDesc``) and
reads and writes each field by its declared kind. Structural truth is the host's current schema;
nothing here keeps a second copy of any vocabulary.

Kinds, as the host numbers them: Int 0, Float 1, Text 2, Bool 3, Bytes 4, Message 5, List 6.
The encoding per kind: Int as a base-10 string, Float as a number (or "NaN" / "Infinity" /
"-Infinity"), Text as a string, Bool as true/false, Bytes as base64, Message as an object of its
fields, List as an array. A field a shape does not declare is refused here, as the gate would.
"""

import base64
import json
import math

INT, FLOAT, TEXT, BOOL, BYTES, MESSAGE, LIST = range(7)
KIND_NAMES = {INT: "Int", FLOAT: "Float", TEXT: "Text", BOOL: "Bool", BYTES: "Bytes",
              MESSAGE: "Message", LIST: "List"}


class ShapeError(ValueError):
    """A value does not fit the shape it claims -- said here, before anything is sent."""


class TypeRef(object):
    __slots__ = ("kind", "ref", "element")

    def __init__(self, kind, ref=None, element=None):
        self.kind = kind
        self.ref = ref          # (name, version) for a Message
        self.element = element  # TypeRef for a List

    def label(self):
        if self.kind == MESSAGE:
            return "%s v%d" % self.ref
        if self.kind == LIST:
            return "List<%s>" % self.element.label()
        return KIND_NAMES.get(self.kind, "?")


class Field(object):
    __slots__ = ("name", "required", "type")

    def __init__(self, name, required, type_):
        self.name = name
        self.required = required
        self.type = type_


class Schema(object):
    """A shape as the host declares it: its name, version and fields, in order."""

    def __init__(self, name, version, fields):
        self.name = name
        self.version = version
        self.fields = fields
        self.by_name = dict((f.name, f) for f in fields)

    def describe(self):
        """``[(name, type label, required)]`` -- the structural truth a help view prints."""
        return [(f.name, f.type.label(), f.required) for f in self.fields]


def _type_from_tokens(tokens, i):
    tok = tokens[i]
    kind = int(tok["kind"])
    if kind == LIST:
        element, j = _type_from_tokens(tokens, i + 1)
        return TypeRef(LIST, element=element), j
    if kind == MESSAGE:
        return TypeRef(MESSAGE, ref=(tok["ref_name"], int(tok["ref_version"]))), i + 1
    return TypeRef(kind), i + 1


def schema_from_desc(fields_json):
    """A Schema from the fields of a ``zen.SchemaDesc`` envelope (the bridge's Schema reply)."""
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
        if k == MESSAGE:
            return self.encode(self.resolve(t.ref[0], t.ref[1]), v, path + ".")
        if k == LIST:
            if not isinstance(v, (list, tuple)):
                raise ShapeError("%s: a list, not %r" % (path, type(v)))
            return [self._cell(e, t.element, "%s[%d]" % (path, i)) for i, e in enumerate(v)]
        raise ShapeError("%s: kind %d is not one this client knows" % (path, k))

    # ---- compat JSON -> Python ------------------------------------------------------------

    def decode(self, schema, fields_json):
        out = {}
        for f in schema.fields:
            if f.name in fields_json:
                out[f.name] = self._value(fields_json[f.name], f.type)
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
        if k == MESSAGE:
            if node is None:
                return None
            return self.decode(self.resolve(t.ref[0], t.ref[1]), node)
        if k == LIST:
            return [self._value(e, t.element) for e in node]
        return node

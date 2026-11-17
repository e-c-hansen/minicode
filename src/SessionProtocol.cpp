// SessionProtocol.cpp — see SessionProtocol.h.
#include "SessionProtocol.h"

#include "EditorTabs.h"

namespace Shared {

namespace {

struct KindName {
    Kind kind;
    const char* name;
};
const KindName kKinds[] = {
    {Kind::Hello, "session.hello"},     {Kind::DocOpen, "doc.open"},
    {Kind::DocReset, "doc.reset"},      {Kind::DocClose, "doc.close"},
    {Kind::DocEdit, "doc.edit"},        {Kind::DocUndo, "doc.undo"},
    {Kind::DocSaved, "doc.saved"},      {Kind::DocWatch, "doc.watch"},
    {Kind::DocQuery, "doc.query"},      {Kind::WindowTabs, "window.tabs"},
    {Kind::WindowClose, "window.close"}, {Kind::StateQuery, "session.state"},
    {Kind::Ping, "ping"},               {Kind::Welcome, "session.welcome"},
    {Kind::DocOpened, "doc.opened"},    {Kind::DocEdited, "doc.edited"},
    {Kind::DocUndone, "doc.undone"},    {Kind::UndoState, "doc.undo-state"},
    {Kind::DocText, "doc.text"},        {Kind::State, "session.state-reply"},
    {Kind::Pong, "pong"},               {Kind::Error, "error"},
};

// A text: UTF-8 when it can be, else its units.
Json textJson(const std::u16string& s) {
    if (!Model::hasLoneSurrogate(s)) return Json(Model::utf8FromUtf16(s));
    Json a = Json::array();
    for (char16_t c : s) a.push(Json((int)c));
    return a;
}

bool textFrom(const Json& j, std::u16string* out) {
    if (j.isString()) {
        *out = Model::utf16FromUtf8(j.asString());
        return true;
    }
    if (!j.isArray()) return false;
    out->clear();
    out->reserve(j.size());
    for (const Json& e : j.elements()) {
        if (!e.isNumber()) return false;
        const double n = e.asNumber();
        if (n < 0 || n > 0xFFFF) return false;
        out->push_back((char16_t)n);
    }
    return true;
}

Json opsJson(const Ot::Ops& ops) {
    Json a = Json::array();
    for (const Ot::Op& o : ops) {
        Json j = Json::array();
        j.push(Json((int64_t)o.pos));
        j.push(Json((int64_t)o.del));
        j.push(textJson(o.ins));
        a.push(std::move(j));
    }
    return a;
}

bool opsFrom(const Json& j, Ot::Ops* out) {
    out->clear();
    if (j.isNull()) return true;
    if (!j.isArray()) return false;
    out->reserve(j.size());
    for (const Json& e : j.elements()) {
        if (!e.isArray() || e.size() != 3 || !e[0].isNumber() || !e[1].isNumber() ||
            e[0].asNumber() < 0 || e[1].asNumber() < 0)
            return false;
        Ot::Op o;
        o.pos = (std::size_t)e[0].asInt();
        o.del = (std::size_t)e[1].asInt();
        if (!textFrom(e[2], &o.ins)) return false;
        out->push_back(std::move(o));
    }
    return true;
}

Json selJson(const Selection& s) {
    Json a = Json::array();
    for (const auto& r : s) {
        Json j = Json::array();
        j.push(Json((int64_t)r.anchor));
        j.push(Json((int64_t)r.caret));
        a.push(std::move(j));
    }
    return a;
}

bool selFrom(const Json& j, Selection* out) {
    out->clear();
    if (j.isNull()) return true;
    if (!j.isArray()) return false;
    for (const Json& e : j.elements()) {
        if (!e.isArray() || e.size() != 2 || !e[0].isNumber() || !e[1].isNumber() ||
            e[0].asNumber() < 0 || e[1].asNumber() < 0)
            return false;
        out->push_back({(std::size_t)e[0].asInt(), (std::size_t)e[1].asInt()});
    }
    return true;
}

void setNum(Json& j, const char* key, std::uint64_t n) {
    if (n) j.set(key, Json((int64_t)n));
}

std::uint64_t num(const Json& j, const char* key) {
    const Json& v = j[key];
    return v.isNumber() && v.asNumber() >= 0 ? (std::uint64_t)v.asInt() : 0;
}

}  // namespace

const char* kindName(Kind k) {
    for (const KindName& n : kKinds)
        if (n.kind == k) return n.name;
    return "?";
}

std::string encode(const Message& m) {
    Json j = Json::object();
    j.set("k", Json(kindName(m.kind)));
    setNum(j, "client", m.client);
    setNum(j, "doc", m.doc);
    setNum(j, "window", m.window);
    setNum(j, "req", m.req);
    setNum(j, "base", m.base);
    setNum(j, "v", m.v);
    setNum(j, "seq", m.seq);
    if (m.unit != Model::Unit::Utf16) j.set("unit", Json("codepoint"));
    if (m.flag) j.set("flag", Json(m.flag));
    if (!m.name.empty()) j.set("name", Json(m.name));
    if (!m.text8.empty()) j.set("text8", Json(m.text8));
    if (!m.text16.empty()) j.set("text", textJson(m.text16));
    if (!m.ops.empty()) j.set("ops", opsJson(m.ops));
    if (!m.ops16.empty()) j.set("ops16", opsJson(m.ops16));
    if (m.step.id) {
        Json s = Json::object();
        s.set("id", Json((int64_t)m.step.id));
        s.set("name", Json(m.step.name));
        if (m.step.coalesce) s.set("coalesce", Json(true));
        if (m.step.fresh) s.set("fresh", Json(true));
        if (m.step.open) s.set("before", selJson(m.step.before));
        if (m.step.close) s.set("after", selJson(m.step.after));
        j.set("step", std::move(s));
    }
    if (m.hasSelect) j.set("select", selJson(m.select));
    if (m.outcome != Outcome::Done) j.set("outcome", Json((int)m.outcome));
    if (m.undo.canUndo || m.undo.canRedo || !m.undo.undoName.empty() || !m.undo.redoName.empty()) {
        Json u = Json::object();
        u.set("canUndo", Json(m.undo.canUndo));
        u.set("canRedo", Json(m.undo.canRedo));
        u.set("undoName", Json(m.undo.undoName));
        u.set("redoName", Json(m.undo.redoName));
        j.set("undo", std::move(u));
    }
    if (!m.json.isNull()) j.set("json", m.json);
    return j.dump();
}

bool decode(const std::string& line, Message* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    Json j;
    std::string err;
    if (!Json::parse(line, j, &err)) return fail("not JSON: " + err);
    if (!j.isObject()) return fail("not an object");
    Message m;
    const std::string& k = j["k"].asString();
    bool known = false;
    for (const KindName& n : kKinds)
        if (k == n.name) {
            m.kind = n.kind;
            known = true;
        }
    if (!known) return fail("unknown message \"" + k + "\"");
    m.client = num(j, "client");
    m.doc = num(j, "doc");
    m.window = num(j, "window");
    m.req = num(j, "req");
    m.base = num(j, "base");
    m.v = num(j, "v");
    m.seq = num(j, "seq");
    if (j.has("unit")) {
        const std::string& u = j["unit"].asString();
        if (u == "codepoint") m.unit = Model::Unit::CodePoint;
        else if (u != "utf16") return fail("unknown unit \"" + u + "\"");
    }
    m.flag = (int)j["flag"].asInt();
    m.name = j["name"].asString();
    m.text8 = j["text8"].asString();
    if (j.has("text") && !textFrom(j["text"], &m.text16)) return fail("bad text");
    if (!opsFrom(j["ops"], &m.ops) || !opsFrom(j["ops16"], &m.ops16)) return fail("bad ops");
    if (j.has("step")) {
        const Json& s = j["step"];
        if (!s.isObject()) return fail("bad step");
        m.step.id = num(s, "id");
        m.step.name = s["name"].asString();
        m.step.coalesce = s["coalesce"].asBool();
        m.step.fresh = s["fresh"].asBool();
        m.step.open = s.has("before");
        m.step.close = s.has("after");
        if (!selFrom(s["before"], &m.step.before) || !selFrom(s["after"], &m.step.after))
            return fail("bad selection");
    }
    if (j.has("select")) {
        m.hasSelect = true;
        if (!selFrom(j["select"], &m.select)) return fail("bad selection");
    }
    const int64_t outcome = j["outcome"].asInt();
    if (outcome < 0 || outcome > 2) return fail("bad outcome");
    m.outcome = (Outcome)outcome;
    if (j.has("undo")) {
        const Json& u = j["undo"];
        m.undo.canUndo = u["canUndo"].asBool();
        m.undo.canRedo = u["canRedo"].asBool();
        m.undo.undoName = u["undoName"].asString();
        m.undo.redoName = u["redoName"].asString();
    }
    if (j.has("json")) m.json = j["json"];
    *out = std::move(m);
    return true;
}

Message roundTrip(const Message& m) {
    Message out;
    std::string error;
    if (!decode(encode(m), &out, &error)) {
        out = Message();
        out.kind = Kind::Error;
        out.name = "loopback could not read its own message: " + error;
    }
    return out;
}

Json windowJson(const Model::Window& w) {
    Json j = Json::object();
    j.set("root", Json(w.root()));
    j.set("focused", Json(w.focused()));
    j.set("fraction", Json(w.fraction()));
    Json groups = Json::array();
    for (int g = 0; g < w.groupCount(); g++) {
        Json group = Json::object();
        Json tabs = Json::array();
        for (Model::DocId id : w.tabs(g)) {
            Json t = Json::object();
            t.set("doc", Json((int64_t)id));
            t.set("path", Json(w.path(id)));
            t.set("preview", Json((int)w.preview(id)));
            tabs.push(std::move(t));
        }
        group.set("tabs", std::move(tabs));
        group.set("active", Json((int64_t)w.active(g)));
        groups.push(std::move(group));
    }
    j.set("groups", std::move(groups));
    j.set("closed", Json((int64_t)w.closedCount()));
    return j;
}

}  // namespace Shared

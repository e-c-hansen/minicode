// session_memory_tests.inc — the `host:memory` group of run_tests.cpp: what
// the session's host (SessionHost.h) and a renderer's client
// (SessionClient.h) keep for a large file, counted by the test binary's own
// operator new (AllocCount.h), in bytes, never in time. The GTK port measured
// its app holding a 24 MB file at about 13 times its size, most of it the
// host (a UTF-16 temporary, the text, a second UTF-16 copy as the saved
// text, and the reset's two copies kept in the log) and the client (a copy
// of the reset's UTF-8 kept in the message in flight). The host now keeps
// one UTF-16 copy, whose chunks the saved text shares (Text.h's Frozen),
// and the client nothing of it. MC_HOST_MEMORY_MB sizes the file (4 MB by
// default; the numbers print). Included there, after session_host_tests.inc.

namespace {

// `bytes` of ASCII in lines of 80 bytes (79 and a newline), as the GTK
// port's measurement used.
std::string memoryText(std::size_t bytes) {
    std::string line;
    for (int i = 0; i < 79; i++) line += (char)('a' + i % 26);
    line += '\n';
    std::string s;
    s.reserve(bytes + line.size());
    while (s.size() < bytes) s += line;
    s.resize(bytes);
    return s;
}

Shared::Message hello(Model::Unit u) {
    Shared::Message m;
    m.kind = Shared::Kind::Hello;
    m.unit = u;
    m.name = "memory";
    m.flag = 4 | 8;   // an app that keeps backups and hears DocDirty
    return m;
}

// MB as the printed numbers give them.
double mb(long long bytes) { return (double)bytes / (1 << 20); }

}  // namespace

static void testSessionMemory() {
    GROUP("host:memory");
    {
        std::size_t size = 4u << 20;
        if (const char* s = std::getenv("MC_HOST_MEMORY_MB"))
            if (std::atoi(s) > 0) size = (std::size_t)std::atoi(s) << 20;
        const std::string text = memoryText(size);
        const double n = (double)size;

        // The host alone, opened as the Mac opens a document (DocOpen with
        // its text) and as the GTK port does (an empty DocOpen, then
        // DocReset with the text): what it keeps once the message is gone,
        // and the most it held meanwhile.
        for (int gtk = 0; gtk < 2; gtk++) {
            const long long before = allocLive();
            allocMarkPeak();
            long long kept = 0, high = 0, edited = 0, saved = 0;
            {
                Shared::Host host([](Shared::ClientId, Shared::Message&&) {});
                const Shared::ClientId c = host.connect();
                // By value: each message is gone once handled, as
                // SessionHost's queue lets go of it.
                auto post = [&](Shared::Message m) { host.handle(c, std::move(m)); };
                post(hello(gtk ? Model::Unit::CodePoint : Model::Unit::Utf16));
                Shared::Message open;
                open.kind = Shared::Kind::DocOpen;
                open.doc = 5;
                open.name = "/tmp/memory.txt";
                if (!gtk) open.text8 = text;
                post(std::move(open));
                if (gtk) {
                    Shared::Message reset;
                    reset.kind = Shared::Kind::DocReset;
                    reset.doc = 5;
                    reset.text8 = text;
                    reset.seq = 1;   // as Client::reset sends it, to be acknowledged
                    post(std::move(reset));
                }
                kept = allocLive() - before;
                high = allocPeak() - before;
                CHECK(host.hasDocument(5) && !host.dirty(5));
                CHECK(host.logSize(5) == 0);
                // One keystroke: one chunk is copied, the saved text keeps
                // the old one, and the document is unsaved; taken back, it
                // is clean again (compared, not assumed).
                Shared::Message edit;
                edit.kind = Shared::Kind::DocEdit;
                edit.doc = 5;
                edit.base = host.version(5);
                edit.ops = Ot::Ops{Ot::Op{size / 2, 0, u"k"}};
                edit.step.id = 1;
                edit.step.name = "Typing";
                post(std::move(edit));
                edited = allocLive() - before;
                CHECK(host.dirty(5));
                Shared::Message undo;
                undo.kind = Shared::Kind::DocEdit;
                undo.doc = 5;
                undo.base = host.version(5);
                undo.ops = Ot::Ops{Ot::Op{size / 2, 1, u""}};
                undo.step.id = 2;
                undo.step.name = "Delete";
                post(std::move(undo));
                CHECK(!host.dirty(5));
                // Saved (DocSaved): the saved text is the text's chunks
                // again, not a copy.
                Shared::Message again;
                again.kind = Shared::Kind::DocEdit;
                again.doc = 5;
                again.base = host.version(5);
                again.ops = Ot::Ops{Ot::Op{0, 0, u"x"}};
                again.step.id = 3;
                again.step.name = "Typing";
                post(std::move(again));
                CHECK(host.dirty(5));
                Shared::Message done;
                done.kind = Shared::Kind::DocSaved;
                done.doc = 5;
                done.base = host.version(5);
                post(std::move(done));
                CHECK(!host.dirty(5));
                saved = allocLive() - before;
            }
            std::printf("  host:memory: the host, opened as %s, keeps %.1f MB for %.1f MB (%.2fx), "
                        "%.2fx at most while opening; %.2fx after a keystroke, %.2fx after a save\n",
                        gtk ? "GTK does" : "the Mac does", mb(kept), mb((long long)size), kept / n,
                        high / n, edited / n, saved / n);
            // One UTF-16 copy (2 bytes a byte of ASCII) and the chunks'
            // line starts and counts; before, about 4 (Mac) and 8 (GTK).
            CHECK(kept < 2.3 * n);
            CHECK(edited - kept < 64 * 1024);
            CHECK(saved < 2.3 * n);
            // The UTF-8 itself, the text, and no whole UTF-16 temporary.
            CHECK(high < 3.5 * n);
            CHECK(allocLive() - before < 64 * 1024);   // all of it let go of
        }

        // A client and the host on its own thread, as the GTK port runs
        // them: what both keep, the renderer's own mirror left out.
        {
            const long long before = allocLive();
            allocMarkPeak();
            long long kept = 0, high = 0;
            {
                std::unique_ptr<Shared::Client> a;
                Shared::SessionHost host([&](Shared::ClientId, Shared::Message&& m) { a->deliver(std::move(m)); });
                a = std::make_unique<Shared::Client>(Model::Unit::CodePoint, "gtk");
                a->setKeepsBackups(true);
                const Shared::ClientId id = host.connect();
                a->connect([&](Shared::Message&& m) { host.post(id, std::move(m)); });
                a->open(6, "/tmp/memory.txt", "", nullptr);
                a->reset(6, text);
                CHECK(a->settle());
                CHECK(a->settle());
                kept = allocLive() - before;
                high = allocPeak() - before;
                bool dirty = true;
                std::size_t log = 1;
                host.inspect([&](const Shared::Host& h) {
                    dirty = h.dirty(6);
                    log = h.logSize(6);
                });
                CHECK(!dirty && log == 0);
            }
            std::printf("  host:memory: a client and its host keep %.1f MB for %.1f MB (%.2fx), "
                        "%.2fx at most\n",
                        mb(kept), mb((long long)size), kept / n, high / n);
            CHECK(kept < 2.4 * n);
            CHECK(high < 4.5 * n);
            CHECK(allocLive() - before < 256 * 1024);
        }
    }
}

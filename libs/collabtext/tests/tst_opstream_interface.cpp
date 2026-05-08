/// tst_opstream_interface.cpp — OpStream public interface smoke test
///
/// Proves the OpStream interface is self-sufficient using only angle-bracket
/// includes from <collabtext/>.  No "crdt/…" relative paths.  No Qt internals.
///
/// This is a compilation + instantiation smoke test; no real transport logic.
/// If this file compiles and all tests pass, Task 2.1 is done.

#include <collabtext/OpStream.h>

#include <QTest>

/// Minimal concrete implementation — all four methods are no-ops.
/// Used to verify the interface can be subclassed and instantiated.
class TestOpStream : public CollabText::OpStream {
public:
    void push(const std::string& /*stream_name*/,
              const std::string& /*payload*/) override
    {
        // no-op
    }

    void set_on_inbound(
        std::function<void(const std::string&, uint16_t, const std::string&)>
            /*cb*/) override
    {
        // no-op
    }

    uint64_t lowest_peer_acked_lamport() const override
    {
        return 0;
    }

    void set_on_ack_update(std::function<void(uint64_t)> /*cb*/) override
    {
        // no-op
    }
};

class TestOpStreamInterface : public QObject {
    Q_OBJECT

private slots:

    /// Verify that a concrete subclass can be instantiated via a base pointer.
    void noop_subclass_instantiates()
    {
        TestOpStream impl;
        CollabText::OpStream* iface = &impl;
        QVERIFY(iface != nullptr);
    }

    /// Verify that lowest_peer_acked_lamport() returns 0 with no peers.
    void lowest_peer_acked_lamport_returns_zero()
    {
        TestOpStream impl;
        QCOMPARE(impl.lowest_peer_acked_lamport(), uint64_t(0));
    }

    /// Verify polymorphic dispatch through the base pointer.
    void lowest_peer_acked_lamport_via_base_pointer()
    {
        TestOpStream impl;
        CollabText::OpStream* iface = &impl;
        QCOMPARE(iface->lowest_peer_acked_lamport(), uint64_t(0));
    }

    /// Verify push() and set_on_inbound() compile and can be called
    /// without crashing (no-op implementations).
    void push_and_inbound_compile_and_run()
    {
        TestOpStream impl;
        impl.push("buffer:doc", "{}");

        bool called = false;
        impl.set_on_inbound([&called](const std::string&, uint16_t,
                                      const std::string&) {
            called = true;
        });
        // Inbound callback is stored but never fired in the no-op impl — that's fine.
        QVERIFY(!called);
    }

    /// Verify set_on_ack_update() compiles and can be called (no-op impl).
    void set_on_ack_update_compiles_and_runs()
    {
        TestOpStream impl;
        bool called = false;
        impl.set_on_ack_update([&called](uint64_t) { called = true; });
        // Ack callback is stored but never fired in the no-op impl — that's fine.
        QVERIFY(!called);
    }
};

QTEST_GUILESS_MAIN(TestOpStreamInterface)
#include "tst_opstream_interface.moc"

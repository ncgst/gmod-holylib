#include "teleport_guard.h"
#include <cassert>
#include <functional>
#include <iostream>
#include <thread>

using HolyLib::TeleportGuard::Scope;

int main()
{
	int accepted = 0, blocked = 0;
	std::function<void(unsigned)> paired;
	paired = [&](unsigned entity) {
		Scope scope(entity);
		if (!scope.Entered()) { ++blocked; return; }
		++accepted;
		paired(entity); // Arrival at the enabled return endpoint.
	};
	for (int i = 0; i < 100000; ++i) paired(42);
	assert(accepted == 100000 && blocked == 100000);

	{
		Scope first(42); assert(first.Entered());
		Scope independent(43); assert(independent.Entered());
		Scope loopThroughOtherEntity(42); assert(!loopThroughOtherEntity.Entered());
		Scope reusedIndexDifferentSerial(42 | (1u << 16)); assert(reusedIndexDifferentSerial.Entered());
		std::thread otherThread([] { Scope sameHandle(42); assert(sameHandle.Entered()); });
		otherThread.join();
	}
	{ Scope afterUnwind(42); assert(afterUnwind.Entered()); }
	try { Scope exception(42); assert(exception.Entered()); throw 1; } catch (int) {}
	{ Scope afterException(42); assert(afterException.Entered()); }

	std::function<void(unsigned)> chain = [&](unsigned depth) {
		Scope scope(depth);
		assert(scope.Entered() == (depth < Scope::MaxDepth));
		if (scope.Entered()) chain(depth + 1);
	};
	chain(0);
	{ Scope afterLimit(0); assert(afterLimit.Entered()); }
	std::cout << "teleport guard: 100000 paired cycles, nested entities, slot reuse, thread isolation, unwinding and depth limit passed\n";
}

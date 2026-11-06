"""Executable ownership model for a future DXVK generated-present scheduler.

This is a contract test, not a Vulkan/WSI implementation. It models each
swapchain image + binary semaphore pair as one transaction slot and refuses
reuse until presentation completion has been observed by the owner.
"""
from dataclasses import dataclass
from enum import Enum, auto
import unittest


class State(Enum):
    FREE = auto()
    ACQUIRED = auto()
    SUBMITTED = auto()
    PRESENT_QUEUED = auto()


@dataclass
class Slot:
    image_index: int
    acquire_semaphore: str
    present_semaphore: str
    state: State = State.FREE
    generation: int = 0


class TransactionPool:
    def __init__(self, image_count):
        if image_count < 2:
            raise ValueError("at least two WSI images are required")
        self.slots = [
            Slot(i, f"acquire-{i}", f"present-{i}")
            for i in range(image_count)
        ]

    def acquire(self, image_index):
        slot = self.slots[image_index]
        if slot.state is not State.FREE:
            raise RuntimeError("WSI image/semaphore slot is still owned")
        slot.generation += 1
        slot.state = State.ACQUIRED
        return slot

    def submit(self, slot):
        self._require_live(slot, State.ACQUIRED)
        slot.state = State.SUBMITTED

    def queue_present(self, slot):
        self._require_live(slot, State.SUBMITTED)
        slot.state = State.PRESENT_QUEUED

    def complete_present(self, slot):
        self._require_live(slot, State.PRESENT_QUEUED)
        slot.state = State.FREE

    @staticmethod
    def _require_live(slot, expected):
        if slot.state is not expected:
            raise RuntimeError(f"invalid transaction transition: {slot.state} -> {expected}")


class PresenterTransactionModelTests(unittest.TestCase):
    def setUp(self):
        self.pool = TransactionPool(3)

    def test_two_independent_transactions_do_not_alias(self):
        game = self.pool.acquire(0)
        generated = self.pool.acquire(1)
        self.assertNotEqual(game.acquire_semaphore, generated.acquire_semaphore)
        self.assertNotEqual(game.present_semaphore, generated.present_semaphore)
        self.pool.submit(game)
        self.pool.submit(generated)
        self.pool.queue_present(game)
        self.pool.queue_present(generated)
        self.pool.complete_present(game)
        self.pool.complete_present(generated)
        self.assertTrue(all(s.state is State.FREE for s in self.pool.slots))

    def test_slot_cannot_be_reacquired_before_present_completion(self):
        slot = self.pool.acquire(0)
        self.pool.submit(slot)
        self.pool.queue_present(slot)
        with self.assertRaisesRegex(RuntimeError, "still owned"):
            self.pool.acquire(0)
        self.pool.complete_present(slot)
        self.assertIs(self.pool.acquire(0), slot)

    def test_present_cannot_be_queued_before_submission(self):
        slot = self.pool.acquire(0)
        with self.assertRaisesRegex(RuntimeError, "invalid transaction transition"):
            self.pool.queue_present(slot)

    def test_duplicate_completion_is_rejected(self):
        slot = self.pool.acquire(0)
        self.pool.submit(slot)
        self.pool.queue_present(slot)
        self.pool.complete_present(slot)
        with self.assertRaisesRegex(RuntimeError, "invalid transaction transition"):
            self.pool.complete_present(slot)

    def test_out_of_date_or_resize_requires_retiring_pool_not_reusing_live_slot(self):
        slot = self.pool.acquire(2)
        self.pool.submit(slot)
        self.pool.queue_present(slot)
        # A swapchain-generation change must not silently reset live ownership.
        with self.assertRaisesRegex(RuntimeError, "still owned"):
            self.pool.acquire(2)
        self.pool.complete_present(slot)
        replacement = TransactionPool(2)
        self.assertEqual(len(replacement.slots), 2)
        self.assertTrue(all(s.state is State.FREE for s in replacement.slots))

    def test_single_image_pool_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "at least two"):
            TransactionPool(1)


if __name__ == "__main__":
    unittest.main()

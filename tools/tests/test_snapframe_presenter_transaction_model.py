"""Reference ownership model for a future DXVK generated-present scheduler.

This is a contract test, not a Vulkan/WSI implementation. A slot owns one
swapchain image and its acquire/present binary semaphores until completion.
"""
from dataclasses import dataclass
from enum import Enum, auto
import unittest


class State(Enum):
    FREE = auto()
    ACQUIRED = auto()
    SUBMITTED = auto()
    PRESENT_QUEUED = auto()
    RETIRED = auto()


class PresentResult(Enum):
    SUCCESS = auto()
    OUT_OF_DATE = auto()
    SURFACE_LOST = auto()
    DEVICE_LOST = auto()


@dataclass(frozen=True)
class Token:
    generation: int
    image_index: int
    serial: int


@dataclass
class Slot:
    image_index: int
    acquire_semaphore: str
    present_semaphore: str
    generation: int
    serial: int = 0
    state: State = State.FREE


class TransactionPool:
    """Keeps old generations alive until their outstanding tokens retire."""

    def __init__(self, image_count, generation=1):
        if image_count < 2:
            raise ValueError("at least two WSI images are required")
        self.generation = generation
        self.pools = {
            generation: [
                Slot(i, f"g{generation}-acquire-{i}",
                     f"g{generation}-present-{i}", generation)
                for i in range(image_count)
            ]
        }
        self.retired_generations = set()
        self.device_lost = False

    @property
    def slots(self):
        """Compatibility view of the current generation."""
        return self.pools[self.generation]

    def acquire(self):
        if self.device_lost or self.generation in self.retired_generations:
            return None
        for slot in self.slots:
            if slot.state is State.FREE:
                slot.serial += 1
                slot.state = State.ACQUIRED
                return Token(slot.generation, slot.image_index, slot.serial)
        # Generated work is optional: never block the source present for a slot.
        return None

    def submit(self, token):
        slot = self._resolve(token, State.ACQUIRED)
        slot.state = State.SUBMITTED

    def queue_present(self, token):
        slot = self._resolve(token, State.SUBMITTED)
        slot.state = State.PRESENT_QUEUED

    def complete_present(self, token, result=PresentResult.SUCCESS):
        slot = self._resolve(token, State.PRESENT_QUEUED)
        if result in (PresentResult.OUT_OF_DATE, PresentResult.SURFACE_LOST):
            self.retired_generations.add(token.generation)
        elif result is PresentResult.DEVICE_LOST:
            self.device_lost = True
            self._abandon_all()
            return
        slot.state = State.RETIRED if token.generation in self.retired_generations else State.FREE
        self._collect_retired_generation(token.generation)

    def recreate(self, image_count):
        if image_count < 2:
            raise ValueError("at least two WSI images are required")
        old_generation = self.generation
        self.retired_generations.add(old_generation)
        self.generation += 1
        self.pools[self.generation] = [
            Slot(i, f"g{self.generation}-acquire-{i}",
                 f"g{self.generation}-present-{i}", self.generation)
            for i in range(image_count)
        ]
        self._collect_retired_generation(old_generation)

    def _resolve(self, token, expected):
        slots = self.pools.get(token.generation)
        if slots is None or token.image_index >= len(slots):
            raise RuntimeError("stale transaction generation")
        slot = slots[token.image_index]
        if slot.serial != token.serial or slot.state is not expected:
            raise RuntimeError("invalid or stale transaction token")
        return slot

    def _collect_retired_generation(self, generation):
        if generation == self.generation or generation not in self.retired_generations:
            return
        slots = self.pools.get(generation, [])
        if all(s.state in (State.FREE, State.RETIRED) for s in slots):
            self.pools.pop(generation, None)

    def _abandon_all(self):
        # Device loss makes normal GPU completion impossible; this is teardown,
        # not permission to reuse these handles on a live device.
        for slots in self.pools.values():
            for slot in slots:
                slot.state = State.RETIRED
        self.pools.clear()


class PresenterTransactionModelTests(unittest.TestCase):
    def setUp(self):
        self.pool = TransactionPool(3)

    def _queue(self, token):
        self.pool.submit(token)
        self.pool.queue_present(token)

    def test_game_and_generated_transactions_have_distinct_ownership(self):
        game = self.pool.acquire()
        generated = self.pool.acquire()
        self.assertIsNotNone(game)
        self.assertIsNotNone(generated)
        game_slot = self.pool.pools[game.generation][game.image_index]
        gen_slot = self.pool.pools[generated.generation][generated.image_index]
        self.assertNotEqual(game_slot.acquire_semaphore, gen_slot.acquire_semaphore)
        self.assertNotEqual(game_slot.present_semaphore, gen_slot.present_semaphore)
        self._queue(game)
        self._queue(generated)
        self.pool.complete_present(game)
        self.pool.complete_present(generated)
        self.assertTrue(all(s.state is State.FREE for s in self.pool.slots))

    def test_no_free_slot_skips_generated_frame_without_waiting(self):
        tokens = [self.pool.acquire() for _ in range(3)]
        self.assertTrue(all(token is not None for token in tokens))
        self.assertIsNone(self.pool.acquire())
        self._queue(tokens[0])
        self.pool.complete_present(tokens[0])
        self.assertIsNotNone(self.pool.acquire())

    def test_slot_cannot_be_reacquired_before_completion(self):
        tokens = [self.pool.acquire() for _ in range(3)]
        token = tokens[0]
        self._queue(token)
        self.assertIsNone(self.pool.acquire())
        same_slot = self.pool.pools[token.generation][token.image_index]
        self.assertIs(same_slot.state, State.PRESENT_QUEUED)
        self.pool.complete_present(token)
        self.assertIsNotNone(self.pool.acquire())

    def test_present_cannot_be_queued_before_submission(self):
        token = self.pool.acquire()
        with self.assertRaisesRegex(RuntimeError, "invalid or stale"):
            self.pool.queue_present(token)

    def test_duplicate_completion_is_rejected(self):
        token = self.pool.acquire()
        self._queue(token)
        self.pool.complete_present(token)
        with self.assertRaisesRegex(RuntimeError, "invalid or stale"):
            self.pool.complete_present(token)

    def test_resize_preserves_live_old_generation_until_completion(self):
        token = self.pool.acquire()
        self._queue(token)
        old_generation = token.generation
        self.pool.recreate(2)
        self.assertIn(old_generation, self.pool.pools)
        self.assertEqual(len(self.pool.slots), 2)
        self.assertIsNotNone(self.pool.acquire())
        self.pool.complete_present(token)
        self.assertNotIn(old_generation, self.pool.pools)
        self.assertIsNotNone(self.pool.acquire())

    def test_out_of_date_retires_generation_and_requires_recreate(self):
        token = self.pool.acquire()
        self._queue(token)
        self.pool.complete_present(token, PresentResult.OUT_OF_DATE)
        self.assertIn(token.generation, self.pool.retired_generations)
        self.pool.recreate(3)
        self.assertIsNotNone(self.pool.acquire())

    def test_surface_lost_retires_generation(self):
        token = self.pool.acquire()
        self._queue(token)
        self.pool.complete_present(token, PresentResult.SURFACE_LOST)
        self.assertIn(token.generation, self.pool.retired_generations)

    def test_device_lost_abandons_all_generations_without_reuse(self):
        token = self.pool.acquire()
        self._queue(token)
        self.pool.complete_present(token, PresentResult.DEVICE_LOST)
        self.assertTrue(self.pool.device_lost)
        self.assertEqual(self.pool.pools, {})
        self.assertIsNone(self.pool.acquire())

    def test_stale_token_cannot_complete_reused_slot(self):
        token = self.pool.acquire()
        self._queue(token)
        self.pool.complete_present(token)
        replacement = self.pool.acquire()
        self.assertEqual(replacement.image_index, token.image_index)
        self.assertNotEqual(replacement.serial, token.serial)
        with self.assertRaisesRegex(RuntimeError, "invalid or stale"):
            self.pool.submit(token)

    def test_single_image_pool_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "at least two"):
            TransactionPool(1)





class SourcePrioritySchedulerModel:
    """Admission policy model; not a Vulkan queue or driver non-blocking proof."""

    def __init__(self):
        self.source_pending = 0
        self.generated_running = False
        self.generated_dropped = 0

    def enqueue_source(self):
        self.source_pending += 1

    def source_submitted(self):
        if self.source_pending <= 0:
            raise RuntimeError("no pending source work")
        self.source_pending -= 1

    def try_start_generated(self):
        # Optional FG is admitted only in a source-idle window. It never waits
        # for source work and never creates a backlog of stale interpolated frames.
        if self.source_pending or self.generated_running:
            self.generated_dropped += 1
            return False
        self.generated_running = True
        return True

    def generated_finished(self):
        if not self.generated_running:
            raise RuntimeError("no generated operation is running")
        self.generated_running = False


class SourcePrioritySchedulerTests(unittest.TestCase):
    def setUp(self):
        self.scheduler = SourcePrioritySchedulerModel()

    def test_generated_work_is_rejected_while_source_is_pending(self):
        self.scheduler.enqueue_source()
        self.assertFalse(self.scheduler.try_start_generated())
        self.assertEqual(self.scheduler.generated_dropped, 1)
        self.assertEqual(self.scheduler.source_pending, 1)

    def test_source_queue_is_drained_before_generated_admission(self):
        self.scheduler.enqueue_source()
        self.scheduler.enqueue_source()
        self.assertFalse(self.scheduler.try_start_generated())
        self.scheduler.source_submitted()
        self.assertFalse(self.scheduler.try_start_generated())
        self.scheduler.source_submitted()
        self.assertTrue(self.scheduler.try_start_generated())

    def test_generated_work_never_accumulates_a_backlog(self):
        self.assertTrue(self.scheduler.try_start_generated())
        self.assertFalse(self.scheduler.try_start_generated())
        self.assertEqual(self.scheduler.generated_dropped, 1)
        self.scheduler.generated_finished()
        self.assertTrue(self.scheduler.try_start_generated())

    def test_source_arrival_is_visible_even_when_generated_is_admitted(self):
        self.assertTrue(self.scheduler.try_start_generated())
        self.scheduler.enqueue_source()
        self.assertEqual(self.scheduler.source_pending, 1)
        # This model does not pretend it can preempt a Vulkan call already in
        # progress; production must not label this as a hard latency guarantee.
        self.assertTrue(self.scheduler.generated_running)

if __name__ == "__main__":
    unittest.main()

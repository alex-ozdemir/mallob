#include "cube_worker.hpp"

#include <cassert>

#include "app/sat/console_horde_interface.hpp"
#include "app/sat/hordesat/solvers/cadical.hpp"
#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "util/console.hpp"

CubeWorker::CubeWorker(CubeSetup &setup) : CubeWorkerInterface(setup) {
    // Initialize solver
    SolverSetup solver_setup;
    solver_setup.logger = &_logger;

    // TODO Fill with valid values
    solver_setup.globalId = 0;
    solver_setup.localId = 0;
    solver_setup.jobname = "cube";
    solver_setup.diversificationIndex = 0;

    _solver = std::make_unique<Cadical>(solver_setup);
}

CubeWorker::~CubeWorker() {
    _logger.log(0, "Enter destructor of CubeWorker");

    if (_worker_state == WAITING || _worker_state == FAILED) {
        // Worker was waiting for a message when destruction occured
        _time_waiting_for_msg += _logger.getTime() - _time_of_last_msg;
    }

    _logger.log(0, "Time waiting for messages: %.3f", _time_waiting_for_msg);
}

void CubeWorker::mainLoop() {
    auto lock = _state_mutex.getLock();

    assert(_worker_state == IDLING);

    _worker_state = WAITING;

    while (true) {
        // After the condition is fulfilled, the lock is reaquired
        _state_cond.wait(lock, [&] { return _worker_state == WORKING || _isInterrupted; });

        _logger.log(0, "The main loop continues");

        // Exit main loop
        if (_isInterrupted) {
            _logger.log(0, "Leaving the main loop due to an interruption");
            return;
        }

        // There should be local cubes available now
        assert(!_local_cubes.empty());

        // Start solving the local cubes
        solve();

        if (_result != UNKNOWN) {
            _logger.log(0, "Leaving the main loop because a solution was found");
            return;
        }
    }
}

void CubeWorker::solve() {
    for (Cube &next_local_cube : _local_cubes) {
        auto path = next_local_cube.getPath();

        _logger.log(0, "Started solving a cube");

        if (includesFailedCube(next_local_cube)) {
            _logger.log(0, "Skipped cube");
            continue;
        }

        auto result = _solver->solve(path);

        // Check result
        if (result == SAT) {
            _logger.log(1, "Found a solution: SAT");
            _worker_state = SOLVED;
            _result = SAT;

            return;

        } else if (result == UNKNOWN) {
            _logger.log(1, "Solving interrupted");

            return;

        } else if (result == UNSAT) {
            _logger.log(1, "Cube failed");

            auto failed_assumps = _solver->getFailedAssumptions();

            if (failed_assumps.size() > 0) {
                _logger.log(1, "Added failed cube");

                // At least one assumption failed -> Append to _failed_cubes
                _failed_cubes.emplace_back(failed_assumps.begin(), failed_assumps.end());

            } else {
                _logger.log(1, "Found a solution: UNSAT");

                // Intersection of assumptions and core is empty -> Formula is unsatisfiable
                _worker_state = SOLVED;
                _result = UNSAT;

                return;
            }
        }
    }
    // All cubes were unsatisfiable and always at least one assumption failed
    _worker_state = FAILED;
}

bool CubeWorker::includesFailedCube(Cube &cube) {
    for (Cube &failed_cube : _failed_cubes)
        if (cube.includes(failed_cube)) return true;

    return false;
}

void CubeWorker::startWorking() {
    // Read formula
    for (int lit : *_formula.get()) {
        _solver->addLiteral(lit);
    }

    _worker_thread = std::thread(&CubeWorker::mainLoop, this);
}

void CubeWorker::interrupt() {
    _isInterrupted.store(true);
    // Exit solve if currently solving
    _solver->interrupt();
    // Resume _worker_thread if currently waiting in mainLoop
    _state_cond.notify();
    // This guarantees termination of the mainLoop
}

void CubeWorker::join() {
    _worker_thread.join();
}

void CubeWorker::suspend() {
    _solver->suspend();
}

void CubeWorker::resume() {
    _solver->resume();
}

bool CubeWorker::wantsToCommunicate() {
    if (_worker_state == WAITING) {
        // Worker is waiting to request new cubes
        return true;

    } else if (_worker_state == FAILED) {
        // Worker is waiting to send his failed cubes
        return true;

    } else {
        // Default case
        return false;
    }
}

void CubeWorker::beginCommunication() {
    // Blocks until lock is aquired
    const std::lock_guard<Mutex> lock(_state_mutex);

    if (_worker_state == WAITING) {
        // Put this into if clause because wantsToCommunicate does not change the state and may return true multiple times
        _time_of_last_msg = _logger.getTime();

        _worker_state = REQUESTING;

        _cube_comm.requestCubes();

        _logger.log(0, "Sent requestCubes signal to root");

    } else if (_worker_state == FAILED) {
        // Put this into if clause because wantsToCommunicate does not change the state and may return true multiple times
        _time_of_last_msg = _logger.getTime();

        _worker_state = RETURNING;

        auto serialized_failed_cubes = serializeCubes(_failed_cubes);

        _cube_comm.returnFailedCubes(serialized_failed_cubes);

        _logger.log(0, "Sent %zu failed cubes to root", _failed_cubes.size());
    }
}

void CubeWorker::handleMessage(int source, JobMessage &msg) {
    // Is only called if a message is received so this can be at the start of the message
    _time_waiting_for_msg += _logger.getTime() - _time_of_last_msg;

    if (msg.tag == MSG_SEND_CUBES) {
        auto serialized_cubes = msg.payload;
        auto cubes = unserializeCubes(serialized_cubes);

        _logger.log(0, "Received %zu cubes from root", cubes.size());

        digestSendCubes(cubes);

    } else if (msg.tag == MSG_RECEIVED_FAILED_CUBES) {
        _logger.log(0, "Received receivedFailedCubes signal from root");

        digestReveicedFailedCubes();

    } else {
        // TODO: Throw error
    }
}

void CubeWorker::digestSendCubes(std::vector<Cube> cubes) {
    const std::lock_guard<Mutex> lock(_state_mutex);
    assert(_worker_state == REQUESTING);

    _local_cubes = cubes;

    // Cubes were digested
    // Worker can now work
    _worker_state = WORKING;
    _state_cond.notify();
}

void CubeWorker::digestReveicedFailedCubes() {
    const std::lock_guard<Mutex> lock(_state_mutex);
    assert(_worker_state == RETURNING);

    _failed_cubes.clear();

    // Failed cubes were returned
    // Worker can now request new cubes
    _worker_state = WAITING;
}
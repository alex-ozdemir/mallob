
#include <thread>
#include <cmath>
#include <limits>

#include "assert.h"
#include "app/job.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "comm/mympi.hpp"

Job::Job(const Parameters& params, int commSize, int worldRank, int jobId) :
            _params(params), 
            _id(jobId),
            _name("#" + std::to_string(jobId)),
            _time_of_arrival(Timer::elapsedSeconds()), 
            _state(INACTIVE),
            _job_tree(commSize, worldRank, jobId), 
            _comm(_id, _job_tree, params.jobCommUpdatePeriod()) {
    
    _growth_period = _params.growthPeriod();
    _continuous_growth = _params.continuousGrowth();
    _max_demand = _params.maxDemand();
    _threads_per_job = _params.numThreadsPerProcess();
}

void Job::updateJobTree(int index, int rootRank, int parentRank) {

    if (index == 0) rootRank = -1;
    _name = "#" + std::to_string(_id) + ":" + std::to_string(index);
    _job_tree.update(index, rootRank, parentRank);
}

void Job::commit(const JobRequest& req) {
    assert(getState() != ACTIVE);
    assert(getState() != PAST);
    _commitment = req;
    _job_tree.clearJobNodeUpdates();
    updateJobTree(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
}

void Job::uncommit() {
    assert(getState() != ACTIVE);
    _commitment.reset();
}

void Job::start(const std::shared_ptr<std::vector<uint8_t>>& data) {
    assertState(INACTIVE);
    
    if (_time_of_activation <= 0) _time_of_activation = Timer::elapsedSeconds();
    _time_of_last_limit_check = Timer::elapsedSeconds();
    _volume = 1;

    _description.deserialize(data);
    _priority = _description.getPriority();
    if (_description.getMaxDemand() > 0) {
        // Set max. demand to more restrictive number
        // among global and job-internal limit
        _max_demand = _max_demand == 0 ? 
            _description.getMaxDemand() // no global max. demand defined
            : std::min(_max_demand, _description.getMaxDemand()); // both limits defined
    }
    
    if (_params.sizeLimitPerProcess() > 0 && 
            _threads_per_job * _description.getNumFormulaLiterals() > (size_t)_params.sizeLimitPerProcess()) {
        
        // Solver literal threshold exceeded: reduce number of solvers for this job
        int optNumThreads = std::floor((float)_params.sizeLimitPerProcess() / _description.getNumFormulaLiterals());
        _threads_per_job = std::max(1, optNumThreads);
        log(V2_INFO, "%s : literal threshold exceeded - cut down #threads to %i\n", toStr(), _threads_per_job);
    }

    _has_description = true;
    _state = ACTIVE;

    appl_start();
}

void Job::stop() {
    assertState(ACTIVE);
    _state = INACTIVE;
    appl_stop();
}

void Job::suspend() {
    assertState(ACTIVE);
    _state = SUSPENDED;
    appl_suspend();
    _volume = 0;
    log(V4_VVER, "%s : suspended solver\n", toStr());
}

void Job::resume() {
    assertState(SUSPENDED);
    _state = ACTIVE;
    appl_resume();
    log(V4_VVER, "%s : resumed solving threads\n", toStr());
}

void Job::interrupt() {
    assertState(ACTIVE);
    _state = STANDBY;
    appl_interrupt();
    _job_tree.unsetLeftChild();
    _job_tree.unsetRightChild();
    log(V4_VVER, "%s : interrupted solver\n", toStr());
}

void Job::restart(const std::shared_ptr<std::vector<uint8_t>>& data) {
    assertState(STANDBY);
    _time_of_activation = Timer::elapsedSeconds();
    _time_of_last_limit_check = Timer::elapsedSeconds();
    _volume = 1;
    assert(!isResultTransferPending());
    _result.reset();
    _description.applyUpdate(data);
    _state = ACTIVE;
    appl_restart();
    log(V4_VVER, "%s : restarted solver\n", toStr());
}

void Job::terminate() {
    assert(_state == INACTIVE || _state == STANDBY || log_return_false("State of %s : %s\n", toStr(), jobStateToStr()));
    _state = PAST;
    _volume = 0;

    appl_terminate();

    _job_tree.unsetLeftChild();
    _job_tree.unsetRightChild();

    _time_of_abort = Timer::elapsedSeconds();
    log(V4_VVER, "%s : terminated\n", toStr());
}

bool Job::isDestructible() {
    assert(getState() == PAST);
    return appl_isDestructible();
}

int Job::getDemand(int prevVolume, float elapsedTime) const {
    if (_state == ACTIVE) {
        int commSize = _job_tree.getCommSize();
        int demand; 
        if (_growth_period <= 0) {
            // Immediate growth
            demand = _job_tree.getCommSize();
        } else {
            if (_time_of_activation <= 0) demand = 1;
            else {
                float t = elapsedTime-_time_of_activation;
                
                // Continuous growth
                float numPeriods = t/_growth_period;
                if (!_continuous_growth) {
                    // Discrete periodic growth
                    numPeriods = std::floor(numPeriods);
                    demand = std::min(commSize, (1 << (int)(numPeriods + 1)) - 1);
                } else {
                    // d(0) := 1; d := 2d+1 every <growthPeriod> seconds
                    demand = std::min(commSize, (int)std::pow(2, numPeriods + 1) - 1);
                }
            }
        }

        // Limit demand if desired
        if (_max_demand > 0) {
            demand = std::min(demand, _max_demand);
        }
        return demand;
        
    } else {
        // "frozen"
        return prevVolume;
    }
}

double Job::getTemperature() const {

    double baseTemp = 0.95;
    double decay = 0.99; // higher means slower convergence

    int age = (int) (Timer::elapsedSeconds()-_time_of_activation);
    double eps = 2*std::numeric_limits<double>::epsilon();

    // Start with temperature 1.0, exponentially converge towards baseTemp 
    double temp = baseTemp + (1-baseTemp) * std::pow(decay, age+1);
    
    // Check if machine precision range is reached, if not reached yet
    if (_age_of_const_cooldown < 0 && _last_temperature - temp <= eps) {
        _age_of_const_cooldown = age;
    }
    // Was limit already reached?
    if (_age_of_const_cooldown >= 0) {
        // indefinitely cool down job by machine precision epsilon
        return baseTemp + (1-baseTemp) * std::pow(decay, _age_of_const_cooldown+1) - (age-_age_of_const_cooldown+1)*eps;
    } else {
        // Use normal calculated temperature
        _last_temperature = temp;
        return temp;
    }
}

const JobResult& Job::getResult() {
    if (!_result.has_value()) _result = appl_getResult();
    assert(_result.value().id >= 0); 
    return _result.value();
}

bool Job::wantsToCommunicate() {
    if (_state != ACTIVE) return false;
    if (_comm.wantsToAggregate()) return true;
    return appl_wantsToBeginCommunication();
}

void Job::communicate() {
    if (_comm.isAggregating()) _comm.beginAggregation();
    else appl_beginCommunication();
}

void Job::communicate(int source, JobMessage& msg) {
    if (!_comm.handle(msg)) {
        appl_communicate(source, msg);
    }
}

/*
 * AILLE Framework - Single Header Implementation
 * AI-Load Integrity and Layered Evaluation
 * 
 * PLUG AND PLAY - Just #include this file
 * 
 * Copyright (c) 2025 Don Michael Feeney Jr
 * 
 * USAGE:
 * ------
 * #include "aille.hpp"
 * 
 * AILLE::AILLEEngine engine;
 * std::vector<AILLE::ModelSignal> signals = get_your_model_predictions();
 * AILLE::Decision decision = engine.makeDecision(signals);
 * 
 * No linking, no dependencies, no unnecessary complexity.
 */

#ifndef AILLE_HPP
#define AILLE_HPP

#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <string>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <functional>

namespace AILLE {

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

struct ModelSignal;
struct Decision;
struct AuditRecord;
struct AILLEConfig;
class AILLEEngine;
class AuditLogger;

enum DecisionStatus {
    DECISION_VALID,
    REJECTED_LOW_CONFIDENCE,
    REJECTED_NO_CONSENSUS,
    FALLBACK_ACTIVATED,
    ERROR_NO_MODELS
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct ModelSignal {
    float value;           // Prediction value
    float confidence;      // 0.0-1.0
    uint64_t timestamp_ns;
    int model_id;
    
    ModelSignal() : value(0.0f), confidence(0.0f), timestamp_ns(0), model_id(-1) {}
    
    ModelSignal(float v, float c, int id = 0) 
        : value(v), confidence(c), model_id(id) {
        timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }
};

struct Decision {
    float final_value;
    DecisionStatus status;
    float confidence;
    int models_agreed;
    bool fallback_used;
    uint64_t timestamp_ns;
    std::vector<int> contributing_models;
    std::string reasoning;
    
    Decision() : final_value(0.0f), status(ERROR_NO_MODELS), confidence(0.0f),
                 models_agreed(0), fallback_used(false), timestamp_ns(0) {}
};

struct AILLEConfig {
    float min_confidence_threshold;
    float grace_confidence_threshold;
    int min_models_required;
    float sign_agreement_threshold;
    int fallback_window_size;
    float fallback_position_scale;
    int max_model_count;
    
    AILLEConfig() :
        min_confidence_threshold(0.35f),
        grace_confidence_threshold(0.25f),
        min_models_required(2),
        sign_agreement_threshold(0.66f),
        fallback_window_size(50),
        fallback_position_scale(0.1f),
        max_model_count(10) {}
};

// ============================================================================
// AILLE ENGINE
// ============================================================================

class AILLEEngine {
private:
    AILLEConfig config;
    std::deque<float> fallback_buffer;
    
    float calculateFallbackValue() const {
        if (fallback_buffer.empty()) return 0.0f;
        float sum = 0.0f;
        for (float val : fallback_buffer) sum += val;
        return sum / fallback_buffer.size();
    }
    
    void updateFallbackBuffer(float value) {
        fallback_buffer.push_back(value);
        while (fallback_buffer.size() > static_cast<size_t>(config.fallback_window_size)) {
            fallback_buffer.pop_front();
        }
    }
    
    float smoothPosition(float signal, float scale = 100.0f) const {
        return std::tanh(signal * scale);
    }
    
    std::vector<ModelSignal> applySafetyLayer(const std::vector<ModelSignal>& signals) {
        std::vector<ModelSignal> valid;
        for (const auto& sig : signals) {
            if (sig.confidence >= config.min_confidence_threshold) {
                valid.push_back(sig);
            } else if (sig.confidence >= config.grace_confidence_threshold) {
                ModelSignal grace_sig = sig;
                grace_sig.confidence *= 0.8f;
                valid.push_back(grace_sig);
            }
        }
        return valid;
    }
    
    bool checkConsensus(const std::vector<ModelSignal>& valid_signals,
                       float& consensus_value, int& models_agreed) {
        if (valid_signals.size() < static_cast<size_t>(config.min_models_required)) {
            models_agreed = 0;
            return false;
        }
        
        std::vector<float> values;
        for (const auto& sig : valid_signals) values.push_back(sig.value);
        
        std::vector<float> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        float median = sorted[sorted.size() / 2];
        float median_sign = (median >= 0) ? 1.0f : -1.0f;
        
        int agreement_count = 0;
        for (float val : values) {
            if (((val >= 0) ? 1.0f : -1.0f) == median_sign) agreement_count++;
        }
        
        models_agreed = agreement_count;
        float agreement_ratio = static_cast<float>(agreement_count) / values.size();
        
        if (agreement_ratio >= config.sign_agreement_threshold && 
            agreement_count >= config.min_models_required) {
            float sum = 0.0f;
            int count = 0;
            for (const auto& sig : valid_signals) {
                if (((sig.value >= 0) ? 1.0f : -1.0f) == median_sign) {
                    sum += sig.value;
                    count++;
                }
            }
            consensus_value = sum / count;
            return true;
        }
        return false;
    }
    
    float getFallbackValue() const {
        float fb = calculateFallbackValue();
        return ((fb >= 0) ? 1.0f : -1.0f) * config.fallback_position_scale;
    }
    
public:
    AILLEEngine() {}
    explicit AILLEEngine(const AILLEConfig& cfg) : config(cfg) {}
    
    Decision makeDecision(const std::vector<ModelSignal>& model_signals) {
        Decision decision;
        decision.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
        
        if (model_signals.empty()) {
            decision.status = ERROR_NO_MODELS;
            decision.reasoning = "No model inputs";
            return decision;
        }
        
        std::vector<ModelSignal> valid = applySafetyLayer(model_signals);
        
        if (valid.empty()) {
            decision.status = REJECTED_LOW_CONFIDENCE;
            decision.final_value = getFallbackValue();
            decision.confidence = 0.1f;
            decision.fallback_used = true;
            decision.reasoning = "All models failed confidence - fallback";
            return decision;
        }
        
        float consensus_value;
        int models_agreed;
        bool consensus_ok = checkConsensus(valid, consensus_value, models_agreed);
        
        if (!consensus_ok) {
            decision.status = REJECTED_NO_CONSENSUS;
            decision.final_value = getFallbackValue();
            decision.confidence = 0.2f;
            decision.fallback_used = true;
            decision.models_agreed = models_agreed;
            decision.reasoning = "No consensus - fallback";
            return decision;
        }
        
        decision.status = DECISION_VALID;
        decision.final_value = smoothPosition(consensus_value);
        
        float total_conf = 0.0f;
        for (const auto& sig : valid) {
            total_conf += sig.confidence;
            decision.contributing_models.push_back(sig.model_id);
        }
        decision.confidence = total_conf / valid.size();
        decision.models_agreed = models_agreed;
        decision.fallback_used = false;
        decision.reasoning = "Consensus: " + std::to_string(models_agreed) + " models";
        
        updateFallbackBuffer(decision.final_value);
        return decision;
    }
    
    void reset() { fallback_buffer.clear(); }
    AILLEConfig getConfig() const { return config; }
    void setConfig(const AILLEConfig& cfg) { config = cfg; }
};

// ============================================================================
// AUDIT LOGGER (OPTIONAL - For Compliance)
// ============================================================================

struct AuditRecord {
    uint64_t timestamp_ns;
    uint64_t decision_id;
    DecisionStatus status;
    float final_value;
    float confidence;
    int models_agreed;
    bool fallback_used;
    std::string reasoning;
    std::string symbol;
    std::string strategy_id;
    std::string hash;
    std::string prev_hash;
    
    AuditRecord() : timestamp_ns(0), decision_id(0), status(DECISION_VALID),
                   final_value(0.0f), confidence(0.0f), models_agreed(0),
                   fallback_used(false) {}
};

class AuditLogger {
private:
    std::ofstream log_file;
    std::vector<AuditRecord> audit_trail;
    uint64_t next_decision_id;
    std::string last_hash;
    
    std::string computeHash(const AuditRecord& record) const {
        std::stringstream ss;
        ss << record.timestamp_ns << record.decision_id << record.final_value
           << record.confidence << record.reasoning << record.prev_hash;
        std::hash<std::string> hasher;
        size_t h = hasher(ss.str());
        std::stringstream hs;
        hs << std::hex << std::setfill('0') << std::setw(16) << h;
        return hs.str();
    }
    
    std::string statusToString(DecisionStatus s) const {
        switch (s) {
            case DECISION_VALID: return "VALID";
            case REJECTED_LOW_CONFIDENCE: return "REJECTED_CONF";
            case REJECTED_NO_CONSENSUS: return "REJECTED_CONSENSUS";
            case FALLBACK_ACTIVATED: return "FALLBACK";
            default: return "ERROR";
        }
    }
    
public:
    AuditLogger() : next_decision_id(1), last_hash("0000000000000000") {}
    
    explicit AuditLogger(const std::string& filename) 
        : next_decision_id(1), last_hash("0000000000000000") {
        open(filename);
    }
    
    ~AuditLogger() { close(); }
    
    bool open(const std::string& filename) {
        log_file.open(filename, std::ios::app);
        if (!log_file.is_open()) return false;
        if (log_file.tellp() == 0) {
            log_file << "timestamp,id,status,value,conf,models,fallback,"
                    << "reasoning,symbol,strategy,hash,prev_hash\n";
        }
        return true;
    }
    
    void close() {
        if (log_file.is_open()) log_file.close();
    }
    
    void logDecision(const Decision& d, const std::string& symbol = "",
                    const std::string& strategy = "") {
        AuditRecord rec;
        rec.timestamp_ns = d.timestamp_ns;
        rec.decision_id = next_decision_id++;
        rec.status = d.status;
        rec.final_value = d.final_value;
        rec.confidence = d.confidence;
        rec.models_agreed = d.models_agreed;
        rec.fallback_used = d.fallback_used;
        rec.reasoning = d.reasoning;
        rec.symbol = symbol;
        rec.strategy_id = strategy;
        rec.prev_hash = last_hash;
        rec.hash = computeHash(rec);
        last_hash = rec.hash;
        
        audit_trail.push_back(rec);
        
        if (log_file.is_open()) {
            time_t sec = rec.timestamp_ns / 1000000000ULL;
            struct tm* ti = gmtime(&sec);
            char buf[80];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ti);
            
            log_file << buf << "," << rec.decision_id << ","
                    << statusToString(rec.status) << "," << rec.final_value << ","
                    << rec.confidence << "," << rec.models_agreed << ","
                    << (rec.fallback_used ? "true" : "false") << ",\""
                    << rec.reasoning << "\"," << rec.symbol << ","
                    << rec.strategy_id << "," << rec.hash << ","
                    << rec.prev_hash << "\n";
            log_file.flush();
        }
    }
    
    bool verifyIntegrity() const {
        if (audit_trail.empty()) return true;
        std::string expected = "0000000000000000";
        for (const auto& rec : audit_trail) {
            if (rec.prev_hash != expected) return false;
            expected = rec.hash;
        }
        return true;
    }
};

} // namespace AILLE

#endif // AILLE_HPP

/*
 * ============================================================================
 * QUICK START EXAMPLE
 * ============================================================================
 * 
 * #include "aille.hpp"
 * 
 * int main() {
 *     // Step 1: Create engine
 *     AILLE::AILLEEngine engine;
 *     
 *     // Step 2: Get your model predictions
 *     std::vector<AILLE::ModelSignal> signals;
 *     signals.push_back(AILLE::ModelSignal(0.05f, 0.85f, 0));  // Model 0
 *     signals.push_back(AILLE::ModelSignal(0.03f, 0.72f, 1));  // Model 1
 *     signals.push_back(AILLE::ModelSignal(0.04f, 0.68f, 2));  // Model 2
 *     
 *     // Step 3: Get validated decision
 *     AILLE::Decision decision = engine.makeDecision(signals);
 *     
 *     // Step 4: Act on it
 *     if (decision.status == AILLE::DECISION_VALID) {
 *         execute_trade(decision.final_value);
 *     }
 *     
 *     return 0;
 * }
 * 
 * ============================================================================
 * COMPILE & RUN
 * ============================================================================
 * 
 * g++ -std=c++17 -O3 your_trading_system.cpp -o trader
 * ./trader
 * 
 * That's it. No linking. No external libraries. Just works.
 */

// Artifact metadata registry (Phase 14) — implementation.

#include "service/artifact.hpp"

#include "service/project.hpp"  // generate_project_id shares the UUID format

namespace vortyx::service {

ServiceStatus validate_artifact_metadata(const ArtifactMetadata& metadata, std::string& error) {
    if (metadata.name.empty() || metadata.name.size() > kMaxProjectNameLength) {
        error = "artifact name must be 1.." + std::to_string(kMaxProjectNameLength) + " bytes";
        return ServiceStatus::InvalidInput;
    }
    for (const char c : metadata.name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) {
            error = "artifact name must not contain control characters";
            return ServiceStatus::InvalidInput;
        }
    }
    if (metadata.declared_byte_size < 0) {
        error = "artifact declared size must be non-negative";
        return ServiceStatus::InvalidInput;
    }
    return ServiceStatus::Ok;
}

ServiceStatus InMemoryArtifactStore::register_artifact(const ArtifactMetadata& request,
                                                       ArtifactMetadata& out) {
    std::string error;
    const ServiceStatus validation = validate_artifact_metadata(request, error);
    if (validation != ServiceStatus::Ok) return validation;

    std::lock_guard<std::mutex> lock(mutex_);
    // The per-project bound (Phase 15): a registry that grows without limit
    // is an unbounded-memory defect. The refusal is a capacity outcome, not
    // a fabricated success.
    std::size_t project_count = 0;
    for (const ArtifactMetadata& artifact : artifacts_) {
        if (artifact.project_id == request.project_id) project_count += 1;
    }
    if (project_count >= kMaxArtifactsPerProject) {
        error = "the project has reached the artifact metadata capacity (" +
                std::to_string(kMaxArtifactsPerProject) + ")";
        return ServiceStatus::Unavailable;
    }
    ArtifactMetadata stored = request;
    stored.artifact_id = generate_project_id();  // the same UUID-v4 id format
    stored.created_at_ms = clock_ ? clock_->now_ms() : 0;
    artifacts_.push_back(stored);
    out = stored;
    return ServiceStatus::Ok;
}

ServiceStatus InMemoryArtifactStore::artifact(const ArtifactId& artifact_id,
                                              ArtifactMetadata& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const ArtifactMetadata& artifact : artifacts_) {
        if (artifact.artifact_id == artifact_id) {
            out = artifact;
            return ServiceStatus::Ok;
        }
    }
    return ServiceStatus::NotFound;
}

ServiceStatus InMemoryArtifactStore::artifacts(const std::string& project_id,
                                               std::vector<ArtifactMetadata>& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out.clear();
    for (const ArtifactMetadata& artifact : artifacts_) {
        if (artifact.project_id == project_id) out.push_back(artifact);
    }
    return ServiceStatus::Ok;
}

ServiceStatus InMemoryArtifactStore::unregister_artifact(const std::string& project_id,
                                                         const ArtifactId& artifact_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = artifacts_.begin(); it != artifacts_.end(); ++it) {
        if (it->artifact_id != artifact_id) continue;
        // Project-scope enforcement: an artifact of ANOTHER project is
        // invisible here (the same anti-enumeration rule as everywhere).
        if (it->project_id != project_id) return ServiceStatus::NotFound;
        artifacts_.erase(it);
        return ServiceStatus::Ok;
    }
    return ServiceStatus::NotFound;
}

std::size_t InMemoryArtifactStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return artifacts_.size();
}

}  // namespace vortyx::service

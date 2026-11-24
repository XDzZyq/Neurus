#pragma once

namespace neurus
{

/**
 * Gives the next UID constructor on this thread the identity already reserved
 * for its archive record. Scope this around a single pooled pointer read.
 */
class ArchiveUid
{
public:
	explicit ArchiveUid(int id);
	~ArchiveUid();

	ArchiveUid(const ArchiveUid&) = delete;
	ArchiveUid& operator=(const ArchiveUid&) = delete;

private:
	int m_previous;
};

} // namespace neurus

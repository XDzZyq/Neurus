#pragma once

namespace neurus
{

/**
 * Gives the next UID constructor on this thread the identity already reserved
 * for its archive record. Scope this around a single pooled pointer read.
 */
class ArchiveUID
{
public:
	explicit ArchiveUID(int id);
	~ArchiveUID();

	ArchiveUID(const ArchiveUID&) = delete;
	ArchiveUID& operator=(const ArchiveUID&) = delete;

private:
	int m_previous;
};

} // namespace neurus

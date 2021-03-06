#include <DataStreams/ReplacingSortedBlockInputStream.h>
#include <Columns/ColumnsNumber.h>
#include <common/logger_useful.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}


void ReplacingSortedBlockInputStream::insertRow(MutableColumns & merged_columns, size_t & merged_rows)
{
    if (out_row_sources_buf)
    {
        /// true flag value means "skip row"
        current_row_sources.back().setSkipFlag(false);

        out_row_sources_buf->write(reinterpret_cast<const char *>(current_row_sources.data()),
                                   current_row_sources.size() * sizeof(RowSourcePart));
        current_row_sources.resize(0);
    }

    ++merged_rows;
    for (size_t i = 0; i < num_columns; ++i)
        merged_columns[i]->insertFrom(*(*selected_row.columns)[i], selected_row.row_num);
}


Block ReplacingSortedBlockInputStream::readImpl()
{
    if (finished)
        return Block();

    if (children.size() == 1)
        return children[0]->read();

    Block header;
    MutableColumns merged_columns;

    init(header, merged_columns);

    if (has_collation)
        throw Exception("Logical error: " + getName() + " does not support collations", ErrorCodes::LOGICAL_ERROR);

    if (merged_columns.empty())
        return Block();

    /// Additional initialization.
    if (selected_row.empty())
    {
        if (!version_column.empty())
            version_column_number = header.getPositionByName(version_column);
    }

    merge(merged_columns, queue);
    return header.cloneWithColumns(std::move(merged_columns));
}


void ReplacingSortedBlockInputStream::merge(MutableColumns & merged_columns, std::priority_queue<SortCursor> & queue)
{
    size_t merged_rows = 0;

    /// Take the rows in needed order and put them into `merged_columns` until rows no more than `max_block_size`
    while (!queue.empty())
    {
        SortCursor current = queue.top();

        if (current_key.empty())
            setPrimaryKeyRef(current_key, current);

        UInt64 version = version_column_number != -1
            ? current->all_columns[version_column_number]->get64(current->pos)
            : 0;

        setPrimaryKeyRef(next_key, current);

        bool key_differs = next_key != current_key;

        /// if there are enough rows and the last one is calculated completely
        if (key_differs && merged_rows >= max_block_size)
            return;

        queue.pop();

        if (key_differs)
        {
            max_version = 0;
            /// Write the data for the previous primary key.
            insertRow(merged_columns, merged_rows);
            current_key.swap(next_key);
        }

        /// Initially, skip all rows. Unskip last on insert.
        if (out_row_sources_buf)
            current_row_sources.emplace_back(current.impl->order, true);

        /// A non-strict comparison, since we select the last row for the same version values.
        if (version >= max_version)
        {
            max_version = version;
            setRowRef(selected_row, current);
        }

        if (!current->isLast())
        {
            current->next();
            queue.push(current);
        }
        else
        {
            /// We get the next block from the corresponding source, if there is one.
            fetchNextBlock(current, queue);
        }
    }

    /// We will write the data for the last primary key.
    insertRow(merged_columns, merged_rows);

    finished = true;
}

}

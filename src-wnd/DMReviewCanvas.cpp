// DarkMark (C) 2019-2021 Stephane Charette <stephanecharette@gmail.com>

#include "DarkMark.hpp"


dm::DMReviewCanvas::DMReviewCanvas(const MReviewInfo & m) :
	mri(m)
{
	const int h = cfg().get_int("review_table_row_height");
	if (h > getRowHeight())
	{
		setRowHeight(h);
	}

	getHeader().addColumn("#"			, 1, 100, 30, -1, TableHeaderComponent::defaultFlags);
	getHeader().addColumn("mark"		, 2, 100, 30, -1, TableHeaderComponent::defaultFlags);
	getHeader().addColumn("size"		, 3, 100, 30, -1, TableHeaderComponent::defaultFlags);
	getHeader().addColumn("aspect ratio", 4, 100, 30, -1, TableHeaderComponent::defaultFlags);
	getHeader().addColumn("rectangle"	, 5, 100, 30, -1, TableHeaderComponent::defaultFlags);
	getHeader().addColumn("overlap"		, 6, 100, 30, -1, TableHeaderComponent::defaultFlags);
	getHeader().addColumn("filename"	, 7, 100, 30, -1, TableHeaderComponent::defaultFlags);
	getHeader().addColumn("type"		, 8, 100, 30, -1, TableHeaderComponent::defaultFlags);
	getHeader().addColumn("message"		, 9, 100, 30, -1, TableHeaderComponent::defaultFlags);
	// if changing columns, also update paintCell() below

	if (cfg().containsKey("review_columns"))
	{
		getHeader().restoreFromString(cfg().get_str("review_columns"));
	}
	else
	{
		getHeader().setStretchToFitActive(true);
	}

	sort_idx.clear();
	sort_idx.reserve(mri.size());
	for (size_t i = 0; i < mri.size(); i ++)
	{
		sort_idx.push_back(i);
	}

	setModel(this);

	return;
}


dm::DMReviewCanvas::~DMReviewCanvas()
{
	cfg().setValue("review_columns", getHeader().toString());

	return;
}


int dm::DMReviewCanvas::getNumRows()
{
	return mri.size();
}


void dm::DMReviewCanvas::cellDoubleClicked(int rowNumber, int columnId, const MouseEvent & event)
{
	// rows are 0-based, columns are 1-based
	if (rowNumber >= 0 and rowNumber < (int)mri.size())
	{
		const auto & review_info = mri.at(sort_idx[rowNumber]);
		const std::string & fn = review_info.filename;

		// we know which image we want to load, but we need the index of that image within the vector of image filenames

		const VStr & image_filenames = dmapp().wnd->content.image_filenames;
		size_t idx = 0;
		while (true)
		{
			if (idx >= image_filenames.size())
			{
				idx = 0;
				break;
			}

			if (image_filenames.at(idx) == fn)
			{
				break;
			}

			idx ++;
		}

//		dmapp().review_wnd->setMinimised(true);
		dmapp().wnd->content.load_image(idx);
		dmapp().wnd->content.highlight_rectangle(review_info.r);
	}

	return;
}


String dm::DMReviewCanvas::getCellTooltip(int rowNumber, int columnId)
{
	// rows are 0-based, columns are 1-based
	if (rowNumber >= 0 and rowNumber < (int)mri.size())
	{
		const auto & review_info = mri.at(sort_idx[rowNumber]);
		const std::string & fn = review_info.filename;

		return "double click to open " + fn;
	}

	return "";
}


void dm::DMReviewCanvas::paintRowBackground(Graphics & g, int rowNumber, int width, int height, bool rowIsSelected)
{
	Colour colour = Colours::white;

	if (rowNumber >= 0 and
		rowNumber < (int)mri.size())
	{
		const auto & review_info = mri.at(sort_idx[rowNumber]);

		if (rowIsSelected)
		{
			colour = Colours::lightblue; // selected rows will have a blue background
		}
		else if (review_info.warnings.size() > 0)
		{
			colour = Colours::lightyellow;
		}
		else if (review_info.errors.size() > 0)
		{
			colour = Colours::lightpink;
		}
	}

	g.fillAll( colour );

	// draw the cell bottom divider between rows
	g.setColour( Colours::black.withAlpha( 0.5f ) );
	g.drawLine( 0, height, width, height );

	return;
}


void dm::DMReviewCanvas::paintCell(Graphics & g, int rowNumber, int columnId, int width, int height, bool rowIsSelected)
{
	if (rowNumber < 0					or
		rowNumber >= (int)mri.size()	or
		columnId < 1					or
		columnId > 9					)
	{
		// rows are 0-based, columns are 1-based
		return;
	}

	auto colour = Colours::black;

	g.setOpacity(1.0);

	const auto & review_info = mri.at(sort_idx[rowNumber]);

	/* columns:
	 *		1: row number
	 *		2: mark (image)
	 *		3: size
	 *		4: aspect ratio
	 *		5: rectangle
	 *		6: overlap
	 *		7: filename
	 *		8: mime type
	 *		9: warning + error messages
	 */

	if (columnId == 2)
	{
		if (review_info.mat.empty() == false)
		{
			// draw a thumbnail of the image
			auto image = convert_opencv_mat_to_juce_image(review_info.mat);
			g.drawImageWithin(image, 0, 0, width, height,
					RectanglePlacement::xLeft				|
					RectanglePlacement::yMid				|
					RectanglePlacement::onlyReduceInSize	);
		}
	}
	else
	{
		std::string str;
		if (columnId == 1)	str = std::to_string(sort_idx[rowNumber] + 1);
		if (columnId == 3)	str = std::to_string(review_info.mat.cols) + " x " + std::to_string(review_info.mat.rows);
		if (columnId == 4)	str = std::to_string(review_info.aspect_ratio);
		if (columnId == 7)	str = review_info.filename;
		if (columnId == 8)	str = review_info.mime_type;

		if (columnId == 5)
		{
			if (review_info.r.area() > 0)
			{
				str =	std::to_string(review_info.r.tl().x) + ", " +
						std::to_string(review_info.r.tl().y) + ", " +
						std::to_string(review_info.r.br().x) + ", " +
						std::to_string(review_info.r.br().y);
			}
		}

		if (columnId == 6)
		{
			const int percentage = std::round(review_info.overlap_sum * 100.0);
			if (percentage > 0)
			{
				str = std::to_string(percentage) + "%";
				if (percentage >= 10)
				{
					colour = Colours::darkred;
				}
			}
		}

		if (columnId == 9)
		{
			for (const auto & msg : review_info.warnings)
			{
				if (str.empty() == false)
				{
					str += "; ";
				}
				str += msg;
			}

			for (const auto & msg : review_info.errors)
			{
				if (str.empty() == false)
				{
					str += "; ";
				}
				str += msg;
			}

			if (str.empty() == false)
			{
				colour = Colours::darkred;
			}
		}

		// draw the text for this cell
		g.setColour(colour);
		Rectangle<int> r(0, 0, width, height);
		g.drawFittedText(str, r.reduced(2), Justification::centredLeft, 1);
	}

	// draw the divider on the right side of the column
	g.setColour(Colours::black.withAlpha(0.5f));
	g.drawLine(width, 0, width, height);

	return;
}


void dm::DMReviewCanvas::sortOrderChanged(int newSortColumnId, bool isForwards)
{
	/* note the sort column is 1-based:
	 *		1: row number
	 *		2: mark (image)
	 *		3: size
	 *		4: aspect ratio
	 *		5: rectangle
	 *		6: overlap
	 *		7: filename
	 *		8: mime type
	 *		9: warning + error messages
	 */

	if (newSortColumnId < 1 or newSortColumnId > 9)
	{
		Log("cannot sort table on invalid column=" + std::to_string(newSortColumnId));
		return;
	}

	if (mri.size() == 0 or sort_idx.size() == 0 or mri.size() != sort_idx.size())
	{
		// nothing to sort!?
		Log("cannot sort table -- invalid number of rows...!?  (mri=" + std::to_string(mri.size()) + ", sort=" + std::to_string(sort_idx.size()) + ")");
		return;
	}

	const size_t class_idx = mri.at(0).class_idx;
	Log("sorting " + std::to_string(sort_idx.size()) + " rows of data for class #" + std::to_string(class_idx) + " using column #" + std::to_string(newSortColumnId));

	std::sort(sort_idx.begin(), sort_idx.end(),
			[&](size_t lhs_idx, size_t rhs_idx)
			{
				if (isForwards == false)
				{
					std::swap(lhs_idx, rhs_idx);
				}

				const auto & lhs_info = mri.at(lhs_idx);
				const auto & rhs_info = mri.at(rhs_idx);

				switch (newSortColumnId)
				{
					case 2:
					case 3:
					{
						const auto lhs_area = lhs_info.mat.cols * lhs_info.mat.rows;
						const auto rhs_area = rhs_info.mat.cols * rhs_info.mat.rows;

						if (lhs_area != rhs_area)
						{
							return lhs_area < rhs_area;
						}
						// if the area is the same, then sort by index
						return lhs_idx < rhs_idx;
					}
					case 4:
					case 5:
					{
						if (lhs_info.aspect_ratio != rhs_info.aspect_ratio)
						{
							return lhs_info.aspect_ratio < rhs_info.aspect_ratio;
						}
						// if the aspect ratio is the same, then sort by index
						return lhs_idx < rhs_idx;
					}
					case 6:
					{
						if (lhs_info.overlap_sum != rhs_info.overlap_sum)
						{
							return lhs_info.overlap_sum < rhs_info.overlap_sum;
						}
						// if the overlap is the same, then sort by index
						return lhs_idx < rhs_idx;
					}
					case 7:
					{
						if (lhs_info.filename != rhs_info.filename)
						{
							return lhs_idx < rhs_idx;
						}
						// if the mime-type is the same, then sort by index
						return lhs_idx < rhs_idx;
					}
					case 8:
					{
						if (lhs_info.mime_type != rhs_info.mime_type)
						{
							return lhs_info.mime_type < rhs_info.mime_type;
						}
						// if the mime-type is the same, then sort by index
						return lhs_idx < rhs_idx;
					}
					case 9:
					{
						if (lhs_info.errors.size()		!= rhs_info.errors.size())		return lhs_info.errors.size()	< rhs_info.errors.size();
						if (lhs_info.warnings.size()	!= rhs_info.warnings.size())	return lhs_info.warnings.size()	< rhs_info.warnings.size();

						// otherwise if there are the same number of errors and warning, sort by mark index
						return lhs_idx < rhs_idx;
					}
					default:
					{
						return lhs_idx < rhs_idx;
					}
				}
			});

	/*
	for (size_t i = 0; i < sort_idx.size(); i ++)
	{
		Log("sort result: at idx=" + std::to_string(i) + " val=" + std::to_string(sort_idx[i]));
	}
	*/

	return;
}

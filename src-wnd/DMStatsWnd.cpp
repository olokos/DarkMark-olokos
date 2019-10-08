/* DarkMark (C) 2019 Stephane Charette <stephanecharette@gmail.com>
 * $Id$
 */

#include "DarkMark.hpp"


dm::DMStatsWnd::DMStatsWnd(DMContent & c) :
		DocumentWindow("DarkMark Statistics", Colours::lightgrey, TitleBarButtons::closeButton),
		content(c)
{
	tlb.getHeader().addColumn("class id"	, 1, 100, 30, -1, TableHeaderComponent::notSortable);
	tlb.getHeader().addColumn("class name"	, 2, 100, 30, -1, TableHeaderComponent::notSortable);
	tlb.getHeader().addColumn("count"		, 3, 100, 30, -1, TableHeaderComponent::notSortable);
	tlb.getHeader().addColumn("images"		, 4, 100, 30, -1, TableHeaderComponent::notSortable);
	tlb.getHeader().addColumn("min size"	, 5, 100, 30, -1, TableHeaderComponent::notSortable);
	tlb.getHeader().addColumn("avg size"	, 6, 100, 30, -1, TableHeaderComponent::notSortable);
	tlb.getHeader().addColumn("max size"	, 7, 100, 30, -1, TableHeaderComponent::notSortable);
	tlb.getHeader().addColumn("SD width"	, 8, 100, 30, -1, TableHeaderComponent::notSortable);
	tlb.getHeader().addColumn("SD height"	, 9, 100, 30, -1, TableHeaderComponent::notSortable);
	// if changing columns, also update paintCell() below

	tlb.getHeader().setStretchToFitActive(true);
	tlb.getHeader().setPopupMenuActive(false);
	tlb.setModel(this);

	setResizable(true, false);
	setContentNonOwned(&tlb, true);

	auto r = dmapp().wnd->getBounds();
	r.reduce(50, 50);
	if (r.getWidth() < 200 or r.getHeight() < 200)
	{
		r = r.withSizeKeepingCentre(600, 400);
	}
	setBounds(r);

	setVisible(true);

	return;
}


dm::DMStatsWnd::~DMStatsWnd()
{
	return;
}


void dm::DMStatsWnd::closeButtonPressed()
{
	// close button

	dmapp().stats_wnd.reset(nullptr);

	return;
}


void dm::DMStatsWnd::userTriedToCloseWindow()
{
	// ALT+F4

	dmapp().stats_wnd.reset(nullptr);

	return;
}


int dm::DMStatsWnd::getNumRows()
{
	return m.size();
}


void dm::DMStatsWnd::cellDoubleClicked(int rowNumber, int columnId, const MouseEvent & event)
{
	// rows are 0-based, columns are 1-based
	if (rowNumber >= 0 and rowNumber < (int)m.size())
	{
		std::string filename;

		const auto & s = m.at(rowNumber);

		if (columnId == 5)	filename = s.min_filename;
		if (columnId == 7)	filename = s.max_filename;

		if (filename.empty() == false)
		{
			for (size_t idx = 0; idx < content.image_filenames.size(); idx ++)
			{
				if (content.image_filenames.at(idx) == filename)
				{
					content.load_image(idx);
					break;
				}
			}
		}
	}

	return;
}


String dm::DMStatsWnd::getCellTooltip(int rowNumber, int columnId)
{
	if (columnId == 3) return "the number of times this object appears across all images";
	if (columnId == 4) return "the number of images where this object appears at least once";
	// 5 is minimum size
	if (columnId == 6) return "the average size of this object (in pixels) across all images";
	// 7 is maximum size
	if (columnId == 8) return "standard deviation of the object's width (in pixels)";
	if (columnId == 9) return "standard deviation of the object's height (in pixels)";

	// rows are 0-based, columns are 1-based
	if (rowNumber >= 0 and rowNumber < (int)m.size())
	{
		const auto & s = m.at(rowNumber);

		if (columnId == 5)
		{
			return "double click to open " + s.min_filename;
		}
		else if (columnId == 7)
		{
			return "double click to open " + s.max_filename;
		}
	}

	return "";

}


void dm::DMStatsWnd::paintRowBackground(Graphics & g, int rowNumber, int width, int height, bool rowIsSelected)
{
	Colour colour = Colours::white;
	if ( rowIsSelected )
	{
		colour = Colours::lightblue; // selected rows will have a blue background
	}
	g.fillAll( colour );

	// draw the cell bottom divider between rows
	g.setColour( Colours::black.withAlpha( 0.5f ) );
	g.drawLine( 0, height, width, height );
	return;
}


void dm::DMStatsWnd::paintCell(Graphics & g, int rowNumber, int columnId, int width, int height, bool rowIsSelected)
{
	if (rowNumber < 0				or
		rowNumber >= (int)m.size()	or
		columnId < 1				or
		columnId > 9				)
	{
		// rows are 0-based, columns are 1-based
		return;
	}

	const auto & s = m.at(rowNumber);

	/* columns:
	 *		1: class id
	 *		2: class name
	 *		3: count
	 *		4: files
	 *		5: min size
	 *		6: avg size
	 *		7: max size
	 *		8: SD width
	 *		9: SD height
	 */
	std::stringstream ss;
	ss << std::fixed << std::setprecision(2);

	switch (columnId)
	{
		case 1: ss << rowNumber;										break;
		case 2: ss << content.names.at(rowNumber);						break;
		case 3: ss << s.count;											break;
		case 4: ss << s.filenames.size();								break;
		case 5: ss << s.min_size.width << " x " << s.min_size.height;	break;
		case 6: ss << s.avg_w << " x " << s.avg_h;						break;
		case 7: ss << s.max_size.width << " x " << s.max_size.height;	break;
		case 8: ss << s.standard_deviation_width;						break;
		case 9: ss << s.standard_deviation_height;						break;
	}

	// draw the text and the right-hand-side dividing line between cells
	g.setColour( Colours::black );
	g.drawFittedText(ss.str(), 0, 0, width, height, Justification::centredLeft, 1 );

	// draw the divider on the right side of the column
	g.setColour( Colours::black.withAlpha( 0.5f ) );
	g.drawLine( width, 0, width, height );

	return;
}

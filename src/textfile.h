/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file textfile.h Code related to textfiles. */

#ifndef TEXTFILE_H
#define TEXTFILE_H

#include <vector>

#include "fileio_type.h"
#include "strings_func.h"
#include "window_gui.h"

/** Additional text files accompanying Tar archives */
enum TextfileType {
	TFT_BEGIN,

	TFT_README = TFT_BEGIN, ///< NewGRF readme
	TFT_CHANGELOG,          ///< NewGRF changelog
	TFT_LICENSE,            ///< NewGRF license

	TFT_END,
};
DECLARE_POSTFIX_INCREMENT(TextfileType)

const char *GetTextfile(TextfileType type, Subdirectory dir, const char *filename);

/** Window for displaying a textfile */
struct TextfileWindow : public Window {
	TextfileType file_type;              ///< Type of textfile to view.
	Scrollbar *vscroll;                  ///< Vertical scrollbar.
	Scrollbar *hscroll;                  ///< Horizontal scrollbar.
	char *text;                          ///< Lines of text from the NewGRF's textfile.
	std::vector<const char *> lines;     ///< #text, split into lines in a table with lines.

	static const int TOP_SPACING    = WD_FRAMETEXT_TOP;    ///< Additional spacing at the top of the #WID_TF_BACKGROUND widget.
	static const int BOTTOM_SPACING = WD_FRAMETEXT_BOTTOM; ///< Additional spacing at the bottom of the #WID_TF_BACKGROUND widget.

	TextfileWindow (TextfileType file_type, const char *textfile, Subdirectory dir);
	virtual ~TextfileWindow();
	virtual void UpdateWidgetSize(int widget, Dimension *size, const Dimension &padding, Dimension *fill, Dimension *resize);
	virtual void OnClick(Point pt, int widget, int click_count);
	virtual void DrawWidget(const Rect &r, int widget) const;
	virtual void OnResize();

private:
	struct GlyphSearcher : std::vector<const char *>::const_iterator, ::MissingGlyphSearcher {
		const std::vector<const char *>::const_iterator begin;
		const std::vector<const char *>::const_iterator end;
		std::vector<const char *>::const_iterator iter;

		GlyphSearcher (const TextfileWindow &tfw)
			: MissingGlyphSearcher (FS_MONO, true),
			  begin (tfw.lines.begin()),
			  end   (tfw.lines.end()),
			  iter  (tfw.lines.begin())
		{
		}

		void Reset() OVERRIDE;
		const char *NextString() OVERRIDE;
	};

	uint GetContentHeight();
	void SetupScrollbars();

public:
	void CheckForMissingGlyphs (void) const
	{
		GlyphSearcher searcher (*this);
		::CheckForMissingGlyphs (true, &searcher);
	}
};

#endif /* TEXTFILE_H */

/*************************************************************************
*
*  HMNJam24
*  Copyright 2024 Martin Fouilleul
*
**************************************************************************/
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _USE_MATH_DEFINES //NOTE: necessary for MSVC
#include <math.h>

#include "orca.h"

oc_font create_font()
{
    //NOTE(martin): create font
    oc_arena_scope scratch = oc_scratch_begin();
    oc_str8 fontPath = oc_path_executable_relative(scratch.arena, OC_STR8("../resources/Menlo.ttf"));
    char* fontPathCString = oc_str8_to_cstring(scratch.arena, fontPath);

    FILE* fontFile = fopen(fontPathCString, "r");
    if(!fontFile)
    {
        oc_log_error("Could not load font file '%s': %s\n", fontPathCString, strerror(errno));
        oc_scratch_end(scratch);
        return (oc_font_nil());
    }
    unsigned char* fontData = 0;
    fseek(fontFile, 0, SEEK_END);
    u32 fontDataSize = ftell(fontFile);
    rewind(fontFile);
    fontData = (unsigned char*)malloc(fontDataSize);
    fread(fontData, 1, fontDataSize, fontFile);
    fclose(fontFile);

    oc_unicode_range ranges[5] = { OC_UNICODE_BASIC_LATIN,
                                   OC_UNICODE_C1_CONTROLS_AND_LATIN_1_SUPPLEMENT,
                                   OC_UNICODE_LATIN_EXTENDED_A,
                                   OC_UNICODE_LATIN_EXTENDED_B,
                                   OC_UNICODE_SPECIALS };

    oc_font font = oc_font_create_from_memory(oc_str8_from_buffer(fontDataSize, (char*)fontData), 5, ranges);
    free(fontData);
    oc_scratch_end(scratch);
    return (font);
}

enum
{
    SIDE_PANEL_WIDTH = 150,
};

typedef enum
{
    BB_CELL_HOLE,
    BB_CELL_LIST,
    BB_CELL_SYMBOL,

} bb_cell_kind;

typedef struct bb_cell bb_cell;

struct bb_cell
{
    oc_list_elt parentElt;
    bb_cell* parent;
    oc_list children;
    u32 childCount;

    u64 id;
    bb_cell_kind kind;
    oc_str8 text;

    oc_rect rect;
    f32 lastLineWidth;
};

typedef struct bb_card
{
    oc_list_elt listElt;

    u32 id;
    oc_rect rect;
    oc_rect displayRect;

    bb_cell* root;
} bb_card;

bool bb_cell_has_children(bb_cell* cell)
{
    return (cell->kind == BB_CELL_LIST);
}

bool bb_cell_has_text(bb_cell* cell)
{
    return (cell->kind == BB_CELL_SYMBOL);
}

typedef struct bb_point
{
    bb_cell* parent;
    bb_cell* leftFrom;
    u32 offset;

} bb_point;

typedef struct bb_cell_editor
{
    f32 spaceWidth;
    f32 lineHeight;

    oc_font font;
    f32 fontSize;
    oc_font_metrics fontMetrics;

    bb_card* editedCard;
    bb_point cursor;
    bb_point mark;

} bb_cell_editor;

//------------------------------------------------------------------------------------------
// bb_point helpers
//------------------------------------------------------------------------------------------
bool bb_point_same_cell(bb_point a, bb_point b)
{
    return (a.parent == b.parent
            && a.leftFrom == b.leftFrom);
}

bool bb_point_equal(bb_point a, bb_point b)
{
    return (a.parent == b.parent
            && a.leftFrom == b.leftFrom
            && a.offset == b.offset);
}

bb_cell* bb_point_left_cell(bb_point point)
{
    return (point.leftFrom ? oc_list_prev_entry(point.parent->children, point.leftFrom, bb_cell, parentElt)
                           : oc_list_last_entry(point.parent->children, bb_cell, parentElt));
}

bb_cell* bb_point_right_cell(bb_point point)
{
    return (point.leftFrom ? point.leftFrom : 0);
}

bb_point bb_prev_point(bb_point point)
{
    if(bb_cell_has_text(point.parent)
       && point.offset > 0)
    {
        point.offset = oc_utf8_prev_offset(point.parent->text, point.offset);
    }
    else
    {
        bb_cell* leftSibling = bb_point_left_cell(point);
        if(leftSibling)
        {
            if(bb_cell_has_children(leftSibling)
               || bb_cell_has_text(leftSibling))
            {
                //NOTE(martin): new point is at the end of the left sibling's children list
                point.leftFrom = 0;
                point.parent = leftSibling;

                if(point.parent->kind == BB_CELL_HOLE)
                {
                    point.offset = 0;
                }
                else
                {
                    point.offset = point.parent->text.len;
                }
            }
            else
            {
                //NOTE(martin): new point is before left sibling
                point.leftFrom = leftSibling;
            }
        }
        else if(point.parent->parent)
        {
            //NOTE(martin): new point is before the parent
            point.leftFrom = point.parent;
            point.parent = point.parent->parent;
            point.offset = 0;
        }
    }
    return (point);
}

#define bb_cell_first_child(parent) oc_list_first_entry(((parent)->children), bb_cell, parentElt)
#define bb_cell_last_child(parent) oc_list_last_entry(((parent)->children), bb_cell, parentElt)
#define bb_cell_next_sibling(cell) oc_list_next_entry(((cell)->parent->children), (cell), bb_cell, parentElt)
#define bb_cell_prev_sibling(cell) oc_list_prev_entry(((cell)->parent->children), (cell), bb_cell, parentElt)

bb_point bb_next_point(bb_point point)
{
    if(bb_cell_has_text(point.parent)
       && point.offset < point.parent->text.len
       && point.parent->kind != BB_CELL_HOLE)
    {
        point.offset = oc_utf8_next_offset(point.parent->text, point.offset);
    }
    else if(point.leftFrom)
    {
        if(bb_cell_has_children(point.leftFrom)
           || bb_cell_has_text(point.leftFrom))
        {
            //NOTE(martin): new point is at the begining of right sibling
            point.parent = point.leftFrom;
            point.leftFrom = bb_cell_first_child(point.leftFrom);
            point.offset = 0;
        }
        else
        {
            //NOTE(martin): next point is after right sibling
            point.leftFrom = bb_cell_next_sibling(point.leftFrom);
        }
    }
    else if(point.parent->parent)
    {
        //NOTE(martin): new point is after parent
        point.leftFrom = bb_cell_next_sibling(point.parent);
        point.parent = point.parent->parent;
        point.offset = 0;
    }
    return (point);
}

oc_rect bb_cell_contents_box(bb_cell_editor* editor, bb_cell* cell)
{
    oc_rect r = cell->rect;

    /*
    f32 leftDecoratorW = bb_cell_left_decorator_width(editor, cell);
    f32 rightDecoratorW = bb_cell_right_decorator_width(editor, cell);
    r.x += leftDecoratorW;
    r.w -= (leftDecoratorW + rightDecoratorW);

    if(cell->box.w > cell->lastLineWidth)
    {
        r.w = cell->box.x + cell->box.w - r.x;
    }
    */
    return (r);
}

//------------------------------------------------------------------------------------------
// point <-> display
//------------------------------------------------------------------------------------------

f32 bb_display_offset_for_text_index(bb_cell_editor* editor, oc_str8 text, u32 offset)
{
    oc_str8 leftText = oc_str8_slice(text, 0, offset);
    oc_text_metrics metrics = oc_font_text_metrics(editor->font, editor->fontSize, leftText);
    return (metrics.logical.w);
}

oc_vec2 bb_point_to_display_pos(bb_cell_editor* editor, bb_point point)
{
    f32 lineHeight = editor->lineHeight;
    oc_vec2 cursorPos = { 0 };

    if(point.leftFrom)
    {
        cursorPos.x = point.leftFrom->rect.x;
        cursorPos.y = point.leftFrom->rect.y;
    }
    else if(bb_cell_has_text(point.parent))
    {
        oc_rect box = bb_cell_contents_box(editor, point.parent);
        cursorPos.x = box.x + bb_display_offset_for_text_index(editor, point.parent->text, point.offset);
        cursorPos.y = box.y;
    }
    else if(!oc_list_empty(point.parent->children))
    {
        bb_cell* leftSibling = oc_list_last_entry(point.parent->children, bb_cell, parentElt);
        cursorPos.x = leftSibling->rect.x + leftSibling->lastLineWidth;
        cursorPos.y = leftSibling->rect.y + leftSibling->rect.h - lineHeight;
    }
    else
    {
        oc_rect box = bb_cell_contents_box(editor, point.parent);
        cursorPos.x = box.x;
        cursorPos.y = box.y;
    }
    return (cursorPos);
}

//------------------------------------------------------------------------------------------
// Moves
//------------------------------------------------------------------------------------------
typedef enum
{
    BB_PREV,
    BB_NEXT
} bb_direction;

void bb_move_one(bb_cell_editor* editor, bb_direction direction)
{
    editor->cursor = (direction == BB_PREV)
                       ? bb_prev_point(editor->cursor)
                       : bb_next_point(editor->cursor);
}

void bb_move_vertical(bb_cell_editor* editor, bb_direction direction)
{
    //NOTE(martin): intercept vertical moves if completion panel is focused
    /*
    if(editor->completionPanel.active)
    {
        return;
    }
    */

    //NOTE(martin): we need to get the x coordinate of the current cursor, then move to next/prev until we find a box
    bb_point point = editor->cursor;
    oc_vec2 oldCursorPos = bb_point_to_display_pos(editor, point);
    oc_vec2 cursorPos = oldCursorPos;
    f32 lineY = cursorPos.y;
    u32 lineCount = 0;

    //TODO: use boxes broad phase first, don't call prev/next point each time?
    while(1)
    {
        bb_point oldPoint = point;

        point = (direction == BB_PREV) ? bb_prev_point(point) : bb_next_point(point);

        if(bb_point_equal(oldPoint, point))
        {
            editor->cursor = point;
            break;
        }
        cursorPos = bb_point_to_display_pos(editor, point);

        if((direction == BB_PREV && cursorPos.y < lineY)
           || (direction == BB_NEXT && cursorPos.y > lineY))
        {
            lineY = cursorPos.y;
            lineCount++;
        }

        if(lineCount > 1)
        {
            editor->cursor = oldPoint;
            break;
        }

        bool condition = (direction == BB_PREV) ? (cursorPos.y < oldCursorPos.y && cursorPos.x <= oldCursorPos.x) : (cursorPos.y > oldCursorPos.y && cursorPos.x >= oldCursorPos.x);

        if(condition)
        {
            editor->cursor = point;
            break;
        }
    }
}

typedef void (*bb_move)(bb_cell_editor* editor, bb_direction direction);
typedef void (*bb_action)(bb_cell_editor* editor);

typedef struct bb_command
{
    oc_key_code key;
    oc_utf32 codePoint;
    oc_keymod_flags mods;

    bb_move move;
    bb_direction direction;
    bool setMark;

    bb_action action;
    bool focusCursor;
    bool rebuild;
    bool updateCompletion;

} bb_command;

const bb_command BB_COMMANDS[] = {
    //NOTE(martin): move
    {
        .key = OC_KEY_LEFT,
        .move = bb_move_one,
        .direction = BB_PREV,
        .setMark = true,
    },
    {
        .key = OC_KEY_RIGHT,
        .move = bb_move_one,
        .direction = BB_NEXT,
        .setMark = true,
    },
    {
        .key = OC_KEY_UP,
        .move = bb_move_vertical,
        .direction = BB_PREV,
        .setMark = true,
    },
    {
        .key = OC_KEY_DOWN,
        .move = bb_move_vertical,
        .direction = BB_NEXT,
        .setMark = true,
    },
    /*
	//NOTE(martin): move select
	{
		.key = OC_KEY_LEFT,
		.mods = OC_KEYMOD_SHIFT,
		.move = bb_move_one,
		.direction = BB_PREV,
	},
	{
		.key = OC_KEY_RIGHT,
		.mods = OC_KEYMOD_SHIFT,
		.move = bb_move_one,
		.direction = BB_NEXT,
	},
	{
		.key = OC_KEY_UP,
		.mods = OC_KEYMOD_SHIFT,
		.move = bb_move_vertical,
		.direction = BB_PREV,
	},
	{
		.key = OC_KEY_DOWN,
		.mods = OC_KEYMOD_SHIFT,
		.move = bb_move_vertical,
		.direction = BB_NEXT,
	},
	//NOTE(martin): cells insertion
	{
		.key = OC_KEY_PERIOD,
		.mods = OC_KEYMOD_CMD,
		.action = bb_insert_comment,
		.rebuild = true,
		.focusCursor = true,
	},
	{
		.codePoint = '(',
		.action = bb_insert_list,
		.rebuild = true,
		.updateCompletion = true,
		.focusCursor = true,
	},
	{
		.codePoint = '[',
		.action = bb_insert_array,
		.rebuild = true,
		.updateCompletion = true,
		.focusCursor = true,
	},
	{
		.codePoint = ' ',
		.action = bb_insert_placeholder,
		.rebuild = true,
		.updateCompletion = true,
		.focusCursor = true,
	},
	{
		.codePoint = '\"',
		.action = bb_insert_string_literal,
		.rebuild = true,
		.updateCompletion = true,
		.focusCursor = true,
	},
	{
		.codePoint = '\'',
		.action = bb_insert_char_literal,
		.rebuild = true,
		.updateCompletion = true,
		.focusCursor = true,
	},
	{
		.codePoint = '@',
		.action = bb_insert_attr,
		.rebuild = true,
		.updateCompletion = true,
		.focusCursor = true,
	},
	{
		.codePoint = '#',
		.action = bb_insert_ui,
		.rebuild = true,
		.updateCompletion = true,
		.focusCursor = true,
	},

	{
		.key = OC_KEY_5,
		.mods = OC_KEYMOD_CMD,
		.action = bb_parenthesize_span,
		.rebuild = true,
		.updateCompletion = true,
		.focusCursor = true,
	},

	//NOTE(martin): deletion
	{
		.key = OC_KEY_BACKSPACE,
		.move = bb_move_one,
		.direction = BB_PREV,
		.action = bb_delete,
		.rebuild = true,
		.updateCompletion = true,
		.focusCursor = true,
	},

	//NOTE(martin): copy/paste
	{
		.key = OC_KEY_C,
		.mods = OC_KEYMOD_CMD,
		.action = bb_copy,
	},
	{
		.key = OC_KEY_X,
		.mods = OC_KEYMOD_CMD,
		.action = bb_cut,
		.focusCursor = true,
	},
	{
		.key = OC_KEY_V,
		.mods = OC_KEYMOD_CMD,
		.action = bb_paste,
		.rebuild = true,
		.focusCursor = true,
	},

	//NOTE(martin): tree selection commands
	{
		.key = OC_KEY_N,
		.mods = OC_KEYMOD_CMD,
		.action = bb_tab_open_transient,
	},
	{
		.key = OC_KEY_N,
		.mods = OC_KEYMOD_CTRL,
		.action = bb_tab_select_next,
	},
	{
		.key = OC_KEY_K,
		.mods = OC_KEYMOD_CMD,
		.action = bb_tab_close_current,
	},
	{
		.key = OC_KEY_N,
		.mods = OC_KEYMOD_CTRL | OC_KEYMOD_SHIFT,
		.action = bb_select_next_build_module,
	},

	//NOTE(martin): print completion list
	{
		.key = OC_KEY_TAB,
		.action = bb_completion_activate,
	},
	*/
};

const u32 BB_COMMAND_COUNT = sizeof(BB_COMMANDS) / sizeof(bb_command);

void bb_run_command(bb_cell_editor* editor, const bb_command* command)
{
    if(command->move)
    {
        /*
        //NOTE(martin): we special case delete here, so that we don't move if we're deleting a selection
        if(command->action == bb_delete)
        {
            if(bb_point_equal(editor->cursor, editor->mark))
            {
                if(!cell_has_text(editor->cursor.parent))
                {
                    //NOTE(martin): select cells first before deleting them
                    command->move(editor, command->direction);
                    return;
                }
            }
            else
            {
                bb_delete(editor);
                goto rebuild;
                return;
            }
        }
        */

        command->move(editor, command->direction);
        if(command->setMark)
        {
            editor->mark = editor->cursor;
        }
    }

    if(command->action)
    {
        command->action(editor);
    }

    if(command->move || command->focusCursor)
    {
        //        editor->scrollToCursorOnNextFrame = true; //NOTE(martin): requests to constrain scroll to cursor on next frame
        //        bb_reset_cursor_blink(editor);
    }

rebuild:
    if(command->rebuild)
    {
        //        bb_rebuild(editor);
    }

    if(command->updateCompletion)
    {
        //        bb_completion_update(editor);
    }
}

//---------------------------------------------------------

typedef struct cell_layout_options
{
    bool vertical;
} cell_layout_options;

cell_layout_options cell_get_layout_options(bb_cell* cell)
{
    cell_layout_options result = { 0 };

    if(cell->kind == BB_CELL_LIST)
    {
        if(!cell->parent)
        {
            //root
            result = (cell_layout_options){
                .vertical = true,
            };
        }
        //...
    }
    //...

    return (result);
}

typedef struct cell_layout_result
{
    oc_rect rect;
    f32 lastLineWidth;
    bool vertical;

} cell_layout_result;

cell_layout_result cell_update_layout(bb_cell_editor* editor, bb_cell* cell, oc_vec2 pos)
{
    cell_layout_result result = {
        .rect = {
            .x = pos.x,
            .y = pos.y,
            .w = 0,
            .h = editor->lineHeight,
        },
    };

    if(bb_cell_has_text(cell))
    {
        oc_str8 text = OC_STR8(" ");
        if(cell->text.len)
        {
            text = cell->text;
        }
        oc_text_metrics metrics = oc_font_text_metrics(editor->font, editor->fontSize, text);

        result.rect.w = metrics.logical.w;
        result.lastLineWidth = metrics.logical.w;
        cell->lastLineWidth = metrics.logical.w;

        //        result.rect.w += bb_cell_left_decorator_width(editor, cell);
        //        result.rect.w += bb_cell_right_decorator_width(editor, cell);
    }
    else if(bb_cell_has_children(cell))
    {
        //NOTE: first compute dimension of children and layout horizontally
        oc_arena_scope scratch = oc_scratch_begin();

        cell_layout_result* childResults = oc_arena_push_array(scratch.arena, cell_layout_result, cell->childCount);
        u32 childIndex = 0;
        oc_vec2 childPos = { 0 };
        oc_list_for(cell->children, child, bb_cell, parentElt)
        {
            childResults[childIndex] = cell_update_layout(editor, child, childPos);
            childPos.x += childResults[childIndex].rect.w;

            result.rect.w += childResults[childIndex].rect.w;

            if(child != oc_list_last_entry(cell->children, bb_cell, parentElt))
            {
                result.rect.w += editor->spaceWidth;
                childPos.x += editor->spaceWidth;
            }
            result.rect.h = oc_max(result.rect.h, childResults[childIndex].rect.h);
            result.vertical = result.vertical || childResults[childIndex].vertical;

            childIndex++;
        }
        result.lastLineWidth = result.rect.w;

        cell_layout_options options = cell_get_layout_options(cell);

        if(options.vertical || result.vertical) //TODO: max child etc
        {
            //NOTE: relayout vertically
            result.rect.w = 0;
            result.rect.h = 0;

            childPos = (oc_vec2){ 0 };
            oc_list_for(cell->children, child, bb_cell, parentElt)
            {
                child->rect.x = childPos.x;
                child->rect.y = childPos.y;
                childPos.y += editor->lineHeight;

                result.rect.w = oc_max(result.rect.w, child->rect.w);
                result.rect.h += child->rect.h;
            }
            result.lastLineWidth = childPos.x;
        }

        oc_scratch_end(scratch);
    }
    cell->rect = result.rect;
    cell->lastLineWidth = result.lastLineWidth;

    return result;
}

void cell_update_rects(bb_cell* cell, oc_vec2 pos)
{
    cell->rect.x += pos.x;
    cell->rect.y += pos.y;

    oc_list_for(cell->children, child, bb_cell, parentElt)
    {
        cell_update_rects(child, (oc_vec2){ cell->rect.x, cell->rect.y });
    }
}

typedef struct bb_box_draw_proc_data
{
    bb_cell_editor* editor;
    bb_cell* cell;

} bb_box_draw_proc_data;

void bb_box_draw_proc(oc_ui_box* box, void* usr)
{
    bb_box_draw_proc_data* data = (bb_box_draw_proc_data*)usr;
    bb_cell* cell = data->cell;
    bb_cell_editor* editor = data->editor;

    /*
    if(cell->id == 0)
    {
        oc_set_color_rgba(1, 1, 1, 1);
    }
    else if(cell->id == 1)
    {
        oc_set_color_rgba(1, 0, 0, 1);
    }
    else if(cell->id == 2)
    {
        oc_set_color_rgba(0, 1, 0, 1);
    }
    else if(cell->id == 3)
    {
        oc_set_color_rgba(0, 0, 1, 1);
    }
    else
    {
        oc_set_color_rgba(1, 0, 1, 1);
    }

    oc_set_width(1);
    oc_rectangle_stroke(box->rect.x, box->rect.y, box->rect.w, box->rect.h);
    */

    if(cell->text.len)
    {
        oc_set_font(editor->font);
        oc_set_font_size(editor->fontSize);

        oc_move_to(box->rect.x, box->rect.y + editor->fontMetrics.ascent);
        oc_text_outlines(cell->text);

        oc_set_color_rgba(1, 1, 1, 1);
        oc_fill();
    }
}

void build_cell_ui(oc_arena* arena, bb_cell_editor* editor, bb_cell* cell)
{
    oc_arena_scope scratch = oc_scratch_begin_next(arena);
    oc_str8 key = oc_str8_pushf(scratch.arena, "cell-%u", cell->id);

    oc_ui_style_next(&(oc_ui_style){
                         .floating = { true, true },
                         .floatTarget = { cell->rect.x, cell->rect.y },
                         .size.width = { OC_UI_SIZE_PIXELS, cell->rect.w },
                         .size.height = { OC_UI_SIZE_PIXELS, cell->rect.h },
                     },
                     OC_UI_STYLE_SIZE | OC_UI_STYLE_FLOAT);

    oc_ui_box* box = oc_ui_box_make_str8(key, OC_UI_FLAG_DRAW_PROC);

    bb_box_draw_proc_data* data = oc_arena_push_type(arena, bb_box_draw_proc_data);
    data->cell = cell;
    data->editor = editor;
    oc_ui_box_set_draw_proc(box, bb_box_draw_proc, data);

    oc_scratch_end(scratch);

    oc_list_for(cell->children, child, bb_cell, parentElt)
    {
        build_cell_ui(arena, editor, child);
    }
}

void bb_draw_cursor(bb_cell_editor* editor)
{
    oc_vec2 cursorPos = bb_point_to_display_pos(editor, editor->cursor);

    oc_set_color_rgba(0.95, 0.71, 0.25, 1);
    oc_rectangle_fill(cursorPos.x - 2, cursorPos.y, 4, editor->lineHeight);
}

void bb_draw_sibling_marker(bb_cell_editor* editor, bb_cell* cell, oc_color color)
{
    f32 lineHeight = editor->lineHeight;

    oc_set_color(color);
    oc_set_width(4);
    oc_move_to(cell->rect.x, cell->rect.y + cell->rect.h - 2);
    oc_line_to(cell->rect.x + cell->lastLineWidth, cell->rect.y + cell->rect.h - 2);
    oc_stroke();
}

void bb_draw_edit_range(bb_cell_editor* editor)
{
    f32 spaceWidth = editor->spaceWidth;
    f32 lineHeight = editor->lineHeight;

    bb_point cursor = editor->cursor;
    bb_point mark = editor->mark;

    if(!bb_point_same_cell(cursor, mark))
    {
        /*
        //NOTE(martin): multiple nodes are selected, highlight the selection (in blue)
        bb_cell_span span = bb_cell_span_from_points(cursor, mark);
        q_cell* parent = span.start->parent;
        q_cell* stop = cell_next_sibling(span.end);

        mp_aligned_rect startBox = bb_cell_frame_box(span.start);
        mp_aligned_rect box = startBox;
        mp_aligned_rect endBox = startBox;

        for(q_cell* cell = span.start; cell != stop; cell = cell_next_sibling(cell))
        {
            endBox = bb_cell_frame_box(cell);
            box = bb_combined_box(box, endBox);
        }

        mp_aligned_rect firstLine = { startBox.x,
                                      startBox.y,
                                      maximum(0, box.x + box.w - startBox.x),
                                      lineHeight };

        mp_aligned_rect innerSpan = { box.x,
                                      box.y + lineHeight,
                                      box.w,
                                      maximum(0, box.h - 2 * lineHeight) };

        mp_aligned_rect lastLine = { box.x,
                                     box.y + box.h - lineHeight,
                                     maximum(0, endBox.x + endBox.w - box.x),
                                     maximum(0, endBox.y + endBox.h - (box.y + box.h - lineHeight)) };

        mp_graphics_set_color_rgba(graphics, 0.2, 0.2, 1, 1);

        mp_graphics_rectangle_fill(graphics, firstLine.x, firstLine.y, firstLine.w, firstLine.h);
        mp_graphics_rectangle_fill(graphics, innerSpan.x, innerSpan.y, innerSpan.w, innerSpan.h);
        mp_graphics_rectangle_fill(graphics, lastLine.x, lastLine.y, lastLine.w, lastLine.h);

        bb_draw_cursor(gui, editor);
        */
    }
    else
    {
        //NOTE(martin): shade the parent cell of cursor/mark
        oc_rect parentBox = bb_cell_contents_box(editor, cursor.parent);
        oc_set_color_rgba(0.2, 0.2, 0.2, 1);
        oc_rectangle_fill(parentBox.x, parentBox.y, parentBox.w, parentBox.h);

        if(!cursor.leftFrom
           && (bb_cell_has_text(cursor.parent))
           && cursor.offset != mark.offset)
        {
            //NOTE(martin): some text is selected. Highlight the selection (in blue).
            oc_rect box = bb_cell_contents_box(editor, cursor.parent);

            u64 start = oc_min(cursor.offset, mark.offset);
            u64 end = oc_max(cursor.offset, mark.offset);

            oc_str8 string = cursor.parent->text;
            oc_rect leftBox = oc_font_text_metrics(editor->font, editor->fontSize, oc_str8_slice(string, 0, start)).logical;
            oc_rect selBox = oc_font_text_metrics(editor->font, editor->fontSize, oc_str8_slice(string, start, end)).logical;

            selBox.x += box.x + leftBox.w;
            selBox.y = box.y;

            oc_set_color_rgba(0.2, 0.2, 1, 1);
            oc_rectangle_fill(selBox.x, selBox.y, selBox.w, selBox.h);

            //  editor->animatedCursor = bb_point_to_display_pos(editor, tab->cursor);
        }
        else
        {
            bb_draw_cursor(editor);
        }
    }
    //NOTE(martin): underline cells to the left/right of cursor
    bb_cell* leftSibling = bb_point_left_cell(cursor);
    bb_cell* rightSibling = bb_point_right_cell(cursor);

    if(leftSibling)
    {
        bb_draw_sibling_marker(editor, leftSibling, (oc_color){ 0.2, 0.5, 1, 1 });
    }
    if(rightSibling)
    {
        bb_draw_sibling_marker(editor, rightSibling, (oc_color){ 0, 1, 0, 1 });
    }
}

void bb_editor_draw_proc(oc_ui_box* box, void* data)
{
    bb_cell_editor* editor = (bb_cell_editor*)data;

    oc_matrix_push((oc_mat2x3){
        1, 0, box->rect.x,
        0, 1, box->rect.y });

    bb_draw_edit_range(editor);
    oc_matrix_pop();
}

int main()
{
    oc_init();

    oc_rect windowRect = { .x = 100, .y = 100, .w = 1200, .h = 800 };
    oc_window window = oc_window_create(windowRect, OC_STR8("test"), 0);

    oc_rect contentRect = oc_window_get_content_rect(window);

    //NOTE: create renderer, surface, and context

    oc_canvas_renderer renderer = oc_canvas_renderer_create();
    if(oc_canvas_renderer_is_nil(renderer))
    {
        oc_log_error("Error: couldn't create renderer\n");
        return (-1);
    }

    oc_surface surface = oc_canvas_surface_create_for_window(renderer, window);
    if(oc_surface_is_nil(surface))
    {
        oc_log_error("Error: couldn't create surface\n");
        return (-1);
    }

    oc_canvas_context context = oc_canvas_context_create();
    if(oc_canvas_context_is_nil(context))
    {
        oc_log_error("Error: couldn't create canvas\n");
        return (-1);
    }

    oc_font font = create_font();

    oc_ui_context ui = { 0 };
    oc_ui_init(&ui);

    // start app
    oc_window_bring_to_front(window);
    oc_window_focus(window);

    f32 x = 400, y = 300;
    f32 speed = 0;
    f32 dx = speed, dy = speed;
    f64 frameTime = 0;

    oc_list leftList = { 0 };
    oc_list rightList = { 0 };
    oc_list middleList = { 0 };

    bb_card cards[8] = {
        {
            .id = 1,
            .rect = { 0, 0, 200, 200 },
        },
        {
            .id = 2,
            .rect = { 0, 0, 200, 200 },
        },
        {
            .id = 3,
            .rect = { 0, 0, 200, 200 },
        },
        {
            .id = 4,
            .rect = { 0, 0, 200, 200 },
        },
        {
            .id = 5,
            .rect = { 0, 0, 200, 200 },
        },
        {
            .id = 6,
            .rect = { 400, 200, 200, 100 },
        },
        {
            .id = 7,
            .rect = { 700, 250, 100, 100 },
        },
        {
            .id = 8,
            .rect = { 500, 400, 200, 300 },
        },
    };

    for(int i = 0; i < 8; i++)
    {
        cards[i].displayRect = cards[i].rect;
    }

    oc_list_push_back(&leftList, &cards[0].listElt);
    oc_list_push_back(&leftList, &cards[1].listElt);
    oc_list_push_back(&leftList, &cards[2].listElt);

    oc_list_push_back(&rightList, &cards[3].listElt);
    oc_list_push_back(&rightList, &cards[4].listElt);

    oc_list_push_back(&middleList, &cards[5].listElt);
    oc_list_push_back(&middleList, &cards[6].listElt);
    oc_list_push_back(&middleList, &cards[7].listElt);

    oc_arena arena = { 0 };
    oc_arena_init(&arena);

    bb_cell* root = oc_arena_push_type(&arena, bb_cell);
    memset(root, 0, sizeof(bb_cell));
    root->id = 0;
    root->kind = BB_CELL_LIST;

    bb_cell* whenList = oc_arena_push_type(&arena, bb_cell);
    memset(whenList, 0, sizeof(bb_cell));
    whenList->id = 1;
    whenList->parent = root;
    oc_list_push_back(&root->children, &whenList->parentElt);
    root->childCount++;
    whenList->kind = BB_CELL_LIST;

    bb_cell* when = oc_arena_push_type(&arena, bb_cell);
    memset(when, 0, sizeof(bb_cell));
    when->id = 2;
    when->parent = whenList;
    oc_list_push_back(&whenList->children, &when->parentElt);
    whenList->childCount++;
    when->kind = BB_CELL_SYMBOL;
    when->text = OC_STR8("when");

    bb_cell* claimList = oc_arena_push_type(&arena, bb_cell);
    memset(claimList, 0, sizeof(bb_cell));
    claimList->id = 3;
    claimList->parent = root;
    oc_list_push_back(&root->children, &claimList->parentElt);
    root->childCount++;
    claimList->kind = BB_CELL_LIST;

    bb_cell* claim = oc_arena_push_type(&arena, bb_cell);
    memset(claim, 0, sizeof(bb_cell));
    claim->id = 4;
    claim->parent = claimList;
    oc_list_push_back(&claimList->children, &claim->parentElt);
    claimList->childCount++;
    claim->kind = BB_CELL_SYMBOL;
    claim->text = OC_STR8("claim");

    bb_cell* self = oc_arena_push_type(&arena, bb_cell);
    memset(self, 0, sizeof(bb_cell));
    self->id = 5;
    self->parent = claimList;
    oc_list_push_back(&claimList->children, &self->parentElt);
    claimList->childCount++;
    self->kind = BB_CELL_SYMBOL;
    self->text = OC_STR8("self");

    cards[7].root = root;

    f32 cardAnimationTimeConstant = 0.2;

    oc_font_metrics metrics = oc_font_get_metrics(font, 14);
    oc_text_metrics spaceMetrics = oc_font_text_metrics(font, 14, OC_STR8(" "));

    bb_cell_editor editor = {
        .font = font,
        .fontSize = 14,
        .fontMetrics = metrics,
        .lineHeight = metrics.ascent + metrics.descent + metrics.lineGap,
        .spaceWidth = spaceMetrics.logical.w,

        .editedCard = &cards[7],
        .cursor = {
            .parent = self,
            .leftFrom = 0,
            .offset = 3,
        },
        .mark = {
            .parent = self,
            .leftFrom = 0,
            .offset = 3,
        },
    };

    while(!oc_should_quit())
    {
        oc_arena_scope scratch = oc_scratch_begin();

        oc_pump_events(0);
        oc_event* event = 0;
        while((event = oc_next_event(scratch.arena)) != 0)
        {
            oc_ui_process_event(event);

            switch(event->type)
            {
                case OC_EVENT_WINDOW_CLOSE:
                {
                    oc_request_quit();
                }
                break;

                case OC_EVENT_KEYBOARD_KEY:
                {
                }
                break;

                default:
                    break;
            }
        }

        //NOTE(martin): handle keyboard shortcuts
        bool runCommand = false;
        oc_keymod_flags mods = oc_key_mods(&ui.input);
        for(int i = 0; i < BB_COMMAND_COUNT; i++)
        {
            const bb_command* command = &(BB_COMMANDS[i]);

            if(oc_key_press_count(&ui.input, command->key)
               && command->mods == mods)
            {
                bb_run_command(&editor, command);
                runCommand = true;
                break;
            }
        }

        oc_vec2 frameSize = oc_surface_get_size(surface);
        oc_ui_style defaultStyle = { .font = font };
        oc_ui_style_mask defaultMask = OC_UI_STYLE_FONT;

        oc_ui_set_theme(&OC_UI_DARK_THEME);

        oc_ui_frame(frameSize, &defaultStyle, defaultMask)
        {
            oc_ui_style_next(&(oc_ui_style){
                                 .size = {
                                     .width = { OC_UI_SIZE_PARENT, 1 },
                                     .height = { OC_UI_SIZE_PARENT, 1 },
                                 },
                                 .bgColor = OC_UI_DARK_THEME.bg4,
                             },
                             OC_UI_STYLE_SIZE | OC_UI_STYLE_BG_COLOR);

            oc_ui_container("frame", OC_UI_FLAG_DRAW_BACKGROUND)
            {

                static bb_card* dragging = 0;

                //-------------------------------------------------------------------------------------
                //move dragged card and detect dragged card hovering side panels

                bool cardHoveringLeftPanel = false;
                bool cardHoveringRightPanel = false;

                if(dragging)
                {
                    oc_vec2 mousePos = oc_mouse_position(&ui.input);
                    oc_vec2 mouseDelta = oc_mouse_delta(&ui.input);

                    dragging->rect.x += mouseDelta.x;
                    dragging->rect.y += mouseDelta.y;

                    if(mousePos.x < SIDE_PANEL_WIDTH)
                    {
                        cardHoveringLeftPanel = true;
                    }
                    if(mousePos.x > frameSize.x - SIDE_PANEL_WIDTH)
                    {
                        cardHoveringRightPanel = true;
                    }
                }

                //-------------------------------------------------------------------------------------
                //center panel
                oc_ui_style_next(&(oc_ui_style){
                                     .floating = {
                                         .x = true,
                                         .y = true,
                                     },
                                     .floatTarget = {
                                         .x = 0,
                                         .y = 0,
                                     },
                                     .size = {
                                         .width = { OC_UI_SIZE_PARENT, 1 },
                                         .height = { OC_UI_SIZE_PARENT, 1 },
                                     },
                                 },
                                 OC_UI_STYLE_FLOAT | OC_UI_STYLE_SIZE);

                oc_ui_box* canvas = oc_ui_container("center-panel", OC_UI_FLAG_DRAW_BORDER | OC_UI_FLAG_CLICKABLE)
                {
                    oc_ui_sig canvasSig = oc_ui_box_sig(canvas);
                    if(canvasSig.dragging)
                    {
                        canvas->scroll.x -= canvasSig.delta.x;
                        canvas->scroll.y -= canvasSig.delta.y;
                    }

                    oc_ui_container("contents", 0)
                    {
                        oc_list_for(middleList, card, bb_card, listElt)
                        {
                            oc_str8 key = oc_str8_pushf(scratch.arena, "card-%u", card->id);

                            oc_ui_box* box = oc_ui_box_lookup_str8(key);

                            typedef enum resize_side
                            {
                                RESIZE_LEFT = 1,
                                RESIZE_RIGHT = 1 << 1,
                                RESIZE_TOP = 1 << 2,
                                RESIZE_BOTTOM = 1 << 3,
                            } resize_side;

                            static resize_side resizing = 0;
                            bool thumbnailed = false;

                            if(box)
                            {
                                oc_ui_sig sig = oc_ui_box_sig(box);
                                if(sig.pressed)
                                {
                                    if(fabs(sig.mouse.x) < 10)
                                    {
                                        resizing |= RESIZE_LEFT;
                                    }
                                    if(fabs(sig.mouse.x - box->rect.w) < 10)
                                    {
                                        resizing |= RESIZE_RIGHT;
                                    }
                                    if(fabs(sig.mouse.y) < 10)
                                    {
                                        resizing |= RESIZE_TOP;
                                    }
                                    if(fabs(sig.mouse.y - box->rect.h) < 10)
                                    {
                                        resizing |= RESIZE_BOTTOM;
                                    }

                                    if(resizing == 0)
                                    {
                                        dragging = card;
                                        oc_vec2 mousePos = oc_mouse_position(&ui.input);
                                        dragging->rect.x = mousePos.x - sig.mouse.x;
                                        dragging->rect.y = mousePos.y - sig.mouse.y;
                                    }
                                }
                                if(sig.released)
                                {
                                    resizing = 0;
                                }
                                if(sig.dragging && resizing)
                                {
                                    if(resizing & RESIZE_LEFT)
                                    {
                                        card->rect.x += sig.delta.x;
                                        card->rect.w -= sig.delta.x;
                                    }
                                    if(resizing & RESIZE_RIGHT)
                                    {
                                        card->rect.w += sig.delta.x;
                                    }
                                    if(resizing & RESIZE_TOP)
                                    {
                                        card->rect.y += sig.delta.y;
                                        card->rect.h -= sig.delta.y;
                                    }
                                    if(resizing & RESIZE_BOTTOM)
                                    {
                                        card->rect.h += sig.delta.y;
                                    }
                                }
                            }

                            card->displayRect.x = card->rect.x;
                            card->displayRect.y = card->rect.y;

                            if(resizing)
                            {
                                card->displayRect.w = card->rect.w;
                                card->displayRect.h = card->rect.h;
                            }

                            if(card != dragging)
                            {
                                oc_ui_style_next(&(oc_ui_style){
                                                     .size = {
                                                         .width = { OC_UI_SIZE_PIXELS, card->displayRect.w },
                                                         .height = { OC_UI_SIZE_PIXELS, card->displayRect.h },
                                                     },
                                                     .floating = {
                                                         .x = true,
                                                         .y = true,
                                                     },
                                                     .floatTarget = { card->displayRect.x, card->displayRect.y },
                                                     .bgColor = OC_UI_DARK_THEME.bg0,
                                                     .borderColor = OC_UI_DARK_THEME.bg1,
                                                     .borderSize = 2,
                                                     .roundness = 5,
                                                 },
                                                 OC_UI_STYLE_SIZE | OC_UI_STYLE_FLOAT | OC_UI_STYLE_BG_COLOR | OC_UI_STYLE_BORDER_COLOR | OC_UI_STYLE_BORDER_SIZE | OC_UI_STYLE_ROUNDNESS);

                                oc_ui_container_str8(key,
                                                     OC_UI_FLAG_DRAW_BACKGROUND
                                                         | OC_UI_FLAG_DRAW_BORDER
                                                         | OC_UI_FLAG_CLICKABLE
                                                         | OC_UI_FLAG_BLOCK_MOUSE)
                                {
                                    oc_ui_label_str8(key);

                                    oc_ui_box* box = oc_ui_container("cells", OC_UI_FLAG_DRAW_PROC)
                                    {
                                        if(card->root)
                                        {
                                            cell_update_layout(&editor, card->root, (oc_vec2){ 10, 20 });
                                            cell_update_rects(card->root, (oc_vec2){ 0 });
                                            build_cell_ui(scratch.arena, &editor, card->root);
                                        }
                                    }

                                    if(editor.editedCard == card)
                                    {
                                        oc_ui_box_set_draw_proc(box, bb_editor_draw_proc, &editor);
                                    }
                                }
                            }
                        }
                    }
                }

                //-------------------------------------------------------------------------------------
                //left panel
                oc_ui_style_next(&(oc_ui_style){
                                     .floating = {
                                         .x = true,
                                         .y = true,
                                     },
                                     .floatTarget = {
                                         .x = 0,
                                         .y = 0,
                                     },
                                     .size = {
                                         .width = { OC_UI_SIZE_PIXELS, SIDE_PANEL_WIDTH },
                                         .height = { OC_UI_SIZE_PARENT, 1 },
                                     },
                                     .bgColor = OC_UI_DARK_THEME.bg1,
                                 },
                                 OC_UI_STYLE_FLOAT | OC_UI_STYLE_SIZE | OC_UI_STYLE_BG_COLOR);

                oc_ui_box* leftPanelScroll = 0;
                oc_list_elt* insertBefore = 0;

                oc_ui_box* leftPanelOuter = oc_ui_container("left-panel-outer", OC_UI_FLAG_DRAW_BACKGROUND | OC_UI_FLAG_DRAW_BORDER)
                {
                    oc_ui_panel("left-panel", 0)
                    {
                        const f32 margin = 20;
                        const f32 spacing = 20;
                        const f32 thumbnailSize = 100;

                        oc_ui_style_next(&(oc_ui_style){
                                             .size.width = { OC_UI_SIZE_PARENT, 1 },
                                             .layout = {
                                                 .axis = OC_UI_AXIS_Y,
                                                 .margin.x = margin,
                                                 .margin.y = margin,
                                                 .spacing = spacing,
                                                 .align.x = OC_UI_ALIGN_CENTER,
                                                 .align.y = OC_UI_ALIGN_START,
                                             },
                                         },
                                         OC_UI_STYLE_SIZE_WIDTH | OC_UI_STYLE_LAYOUT);

                        leftPanelScroll = oc_ui_box_top()->parent;

                        oc_ui_container("contents", 0)
                        {
                            i32 placeholderIndex = -1;
                            if(cardHoveringLeftPanel)
                            {
                                f32 y = dragging->rect.y + leftPanelScroll->scroll.y - margin / 2;
                                placeholderIndex = (int)(y / (spacing + thumbnailSize));
                            }

                            i32 index = 0;
                            f32 x = (SIDE_PANEL_WIDTH - thumbnailSize) / 2;
                            f32 y = margin;
                            oc_list_for_safe(leftList, card, bb_card, listElt)
                            {
                                if(index == placeholderIndex)
                                {
                                    insertBefore = &card->listElt;
                                    y += thumbnailSize + spacing;
                                }

                                card->displayRect.x += cardAnimationTimeConstant * (x - card->displayRect.x);
                                card->displayRect.y += cardAnimationTimeConstant * (y - card->displayRect.y);
                                card->displayRect.w = 100;
                                card->displayRect.h = 100;

                                oc_str8 key = oc_str8_pushf(scratch.arena, "card-%u", card->id);

                                oc_ui_style_next(&(oc_ui_style){
                                                     .size = {
                                                         .width = { OC_UI_SIZE_PIXELS, 100 },
                                                         .height = { OC_UI_SIZE_PIXELS, 100 },
                                                     },
                                                     .floating = { true, true },
                                                     .floatTarget = { card->displayRect.x, card->displayRect.y },
                                                     .bgColor = OC_UI_DARK_THEME.bg0,
                                                     .borderColor = OC_UI_DARK_THEME.bg1,
                                                     .borderSize = 2,
                                                     .roundness = 5,
                                                 },
                                                 OC_UI_STYLE_SIZE | OC_UI_STYLE_FLOAT | OC_UI_STYLE_BG_COLOR | OC_UI_STYLE_BORDER_COLOR | OC_UI_STYLE_BORDER_SIZE | OC_UI_STYLE_ROUNDNESS);

                                oc_ui_box* box = oc_ui_container_str8(key, OC_UI_FLAG_DRAW_BACKGROUND | OC_UI_FLAG_CLICKABLE)
                                {
                                    oc_ui_label_str8(key);
                                }

                                oc_ui_sig sig = oc_ui_box_sig(box);
                                if(sig.pressed)
                                {
                                    oc_vec2 mousePos = oc_mouse_position(&ui.input);

                                    card->rect.x = mousePos.x - sig.mouse.x;
                                    card->rect.y = mousePos.y - sig.mouse.y;
                                    oc_list_remove(&leftList, &card->listElt);
                                    oc_list_push_back(&middleList, &card->listElt);

                                    dragging = card;
                                }

                                y += thumbnailSize + spacing;
                                index++;
                            }
                        }
                    }
                }

                //-------------------------------------------------------------------------------------
                // dragged card

                if(dragging)
                {

                    oc_vec2 mousePos = oc_mouse_position(&ui.input);

                    bool thumbnailed = (mousePos.x < SIDE_PANEL_WIDTH)
                                    || (mousePos.x > frameSize.x - SIDE_PANEL_WIDTH);

                    //TODO: convert to panel content coordinates, and compute where in the list to insert placeholder
                    // code up there must display it there
                    // if we release the card, animate its position to the placeholder...
                    // also, card on the playground should be _below_ sidebars, but grabbed card should be above

                    if(thumbnailed)
                    {
                        dragging->displayRect.w += cardAnimationTimeConstant * (100 - dragging->displayRect.w);
                        dragging->displayRect.h += cardAnimationTimeConstant * (100 - dragging->displayRect.h);
                    }
                    else
                    {
                        dragging->displayRect.w += cardAnimationTimeConstant * (dragging->rect.w - dragging->displayRect.w);
                        dragging->displayRect.h += cardAnimationTimeConstant * (dragging->rect.h - dragging->displayRect.h);
                    }

                    oc_ui_style_next(&(oc_ui_style){
                                         .size = {
                                             .width = { OC_UI_SIZE_PIXELS, dragging->displayRect.w },
                                             .height = { OC_UI_SIZE_PIXELS, dragging->displayRect.h },
                                         },
                                         .floating = {
                                             .x = true,
                                             .y = true,
                                         },
                                         .floatTarget = { dragging->displayRect.x, dragging->displayRect.y },
                                         .bgColor = OC_UI_DARK_THEME.bg0,
                                         .borderColor = OC_UI_DARK_THEME.bg1,
                                         .borderSize = 2,
                                         .roundness = 5,
                                     },
                                     OC_UI_STYLE_SIZE | OC_UI_STYLE_FLOAT | OC_UI_STYLE_BG_COLOR | OC_UI_STYLE_BORDER_COLOR | OC_UI_STYLE_BORDER_SIZE | OC_UI_STYLE_ROUNDNESS);

                    oc_str8 key = oc_str8_pushf(scratch.arena, "card-%u", dragging->id);

                    oc_ui_container_str8(key,
                                         OC_UI_FLAG_DRAW_BACKGROUND
                                             | OC_UI_FLAG_DRAW_BORDER
                                             | OC_UI_FLAG_CLICKABLE
                                             | OC_UI_FLAG_BLOCK_MOUSE
                                             | (dragging ? OC_UI_FLAG_OVERLAY : 0))
                    {
                        oc_ui_label_str8(key);
                    }
                }

                if(dragging && oc_mouse_released(&ui.input, OC_MOUSE_LEFT))
                {
                    if(cardHoveringLeftPanel)
                    {
                        dragging->rect.x += leftPanelScroll->scroll.x;
                        dragging->rect.y += leftPanelScroll->scroll.y;

                        oc_list_remove(&middleList, &dragging->listElt);
                        if(!insertBefore)
                        {
                            oc_list_push_back(&leftList, &dragging->listElt);
                        }
                        else
                        {
                            oc_list_insert_before(&leftList, insertBefore, &dragging->listElt);
                        }
                    }
                    else
                    {
                        dragging->rect.x += canvas->scroll.x;
                        dragging->rect.y += canvas->scroll.y;
                    }

                    dragging = 0;
                }
            }
        }

        oc_ui_draw();

        oc_canvas_render(renderer, context, surface);
        oc_canvas_present(renderer, surface);

        oc_scratch_end(scratch);
    }

    oc_terminate();

    return (0);
}

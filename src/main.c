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

enum
{
    SIDE_PANEL_WIDTH = 150,
};

typedef enum
{
    BB_CELL_HOLE,
    BB_CELL_KEYWORD,
    BB_CELL_SYMBOL,
    BB_CELL_CHAR,
    BB_CELL_STRING,
    BB_CELL_INT,
    BB_CELL_FLOAT,
    BB_CELL_COMMENT,
    BB_CELL_PLACEHOLDER,

    BB_CELL_LIST,

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
    u64 valU64;
    f64 valF64;

    oc_rect rect;
    f32 lastLineWidth;

    u32 lastFrame;
    u32 lastRun;
};

typedef struct bb_card
{
    oc_list_elt listElt;

    u32 id;
    oc_rect rect;
    oc_rect displayRect;

    bb_cell* root;

    oc_str8 label;
    u32 labelFrame;

    oc_color highlight;
    u32 highlightFrame;

    u32 whiskerFrame[4];
    u32 whiskerBoldFrame[4];
} bb_card;

enum
{
    BB_WHISKER_DIRECTION_UP = 0,
    BB_WHISKER_DIRECTION_LEFT,
    BB_WHISKER_DIRECTION_DOWN,
    BB_WHISKER_DIRECTION_RIGHT,
    BB_WHISKER_DIRECTION_COUNT,
};

f32 BB_WHISKER_SIZE = 100;

bool bb_cell_has_children(bb_cell* cell)
{
    return (cell->kind == BB_CELL_LIST);
}

bool bb_cell_has_text(bb_cell* cell)
{
    return (cell->kind >= BB_CELL_HOLE && cell->kind <= BB_CELL_PLACEHOLDER);
}

typedef struct bb_point
{
    bb_cell* parent;
    bb_cell* leftFrom;
    u32 offset;

} bb_point;

typedef struct bb_cell_editor
{
    oc_arena arena;
    u64 nextCellId;

    f32 spaceWidth;
    f32 lineHeight;

    oc_font font;
    f32 fontSize;
    oc_font_metrics fontMetrics;

    bb_card* editedCard;
    bb_point cursor;
    bb_point mark;

} bb_cell_editor;

bb_cell* bb_cell_alloc(bb_cell_editor* editor, bb_cell_kind kind)
{
    bb_cell* cell = oc_arena_push_type(&editor->arena, bb_cell);
    memset(cell, 0, sizeof(bb_cell));
    cell->id = editor->nextCellId++;
    cell->kind = kind;
    return (cell);
}

void bb_cell_recycle(bb_cell_editor* editor, bb_cell* cell)
{
    if(cell->parent)
    {
        oc_list_remove(&cell->parent->children, &cell->parentElt);
        cell->parent->childCount--;
    }

    oc_list_for_safe(cell->children, child, bb_cell, parentElt)
    {
        bb_cell_recycle(editor, child);
    }

    /*
    if(cell->text.string.ptr)
    {
        free(cell->text.string.ptr);
    }
    mem_pool_recycle(&tree->cellPool, cell);
    */
}

void bb_cell_push(bb_cell* parent, bb_cell* cell)
{
    OC_DEBUG_ASSERT(cell != parent);
    cell->parent = parent;
    oc_list_push_back(&parent->children, &cell->parentElt);
    cell->parent->childCount++;
}

void bb_cell_insert(bb_cell* afterSibling, bb_cell* cell)
{
    OC_DEBUG_ASSERT(cell != afterSibling->parent);
    cell->parent = afterSibling->parent;
    oc_list_insert(&cell->parent->children, &afterSibling->parentElt, &cell->parentElt);
    cell->parent->childCount++;
}

void bb_cell_insert_before(bb_cell* beforeSibling, bb_cell* cell)
{
    OC_DEBUG_ASSERT(cell != beforeSibling->parent);
    cell->parent = beforeSibling->parent;
    oc_list_insert_before(&cell->parent->children, &beforeSibling->parentElt, &cell->parentElt);
    cell->parent->childCount++;
}

void bb_cell_text_replace(bb_cell_editor* editor, bb_cell* cell, oc_str8 string)
{
    cell->text = oc_str8_push_copy(&editor->arena, string);
}

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
    return (point.leftFrom ? oc_list_prev_entry(point.leftFrom, bb_cell, parentElt)
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
#define bb_cell_next_sibling(cell) oc_list_next_entry((cell), bb_cell, parentElt)
#define bb_cell_prev_sibling(cell) oc_list_prev_entry((cell), bb_cell, parentElt)

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

f32 bb_cell_left_decorator_width(bb_cell_editor* editor, bb_cell* cell)
{
    //NOTE(martin): return the width of the left decorator of a cell, which can be '(', '[', '"', '@(', or none.
    //WARN(martin): here we assume that all decorator characters are single width, which may not be true for every font.
    f32 width = 0;
    switch(cell->kind)
    {
        case BB_CELL_LIST:
            if(cell->parent)
            {
                width = editor->spaceWidth;
            }
            break;
        case BB_CELL_STRING:
            width = editor->spaceWidth;
            break;
        case BB_CELL_COMMENT:
            width = 2 * editor->spaceWidth;
            break;
        default:
            break;
    }
    return (width);
}

f32 bb_cell_right_decorator_width(bb_cell_editor* editor, bb_cell* cell)
{
    //NOTE(martin): return the width of the right decorator of a cell, which can be ')', ']', '"' or none.
    //WARN(martin): here we assume that all decorator characters are single width, which may not be true for every font.
    f32 width = 0;

    switch(cell->kind)
    {
        case BB_CELL_LIST:
            if(cell->parent)
            {
                width = editor->spaceWidth;
            }
            break;

        case BB_CELL_STRING:
            width = editor->spaceWidth;
            break;

        case BB_CELL_COMMENT:
            width = 2 * editor->spaceWidth;
            break;
        default:
            break;
    }
    return (width);
}

oc_rect bb_cell_contents_box(bb_cell_editor* editor, bb_cell* cell)
{
    oc_rect r = cell->rect;

    f32 leftDecoratorW = bb_cell_left_decorator_width(editor, cell);
    f32 rightDecoratorW = bb_cell_right_decorator_width(editor, cell);
    r.x += leftDecoratorW;
    r.w -= (leftDecoratorW + rightDecoratorW);

    if(cell->rect.w > cell->lastLineWidth)
    {
        r.w = cell->rect.x + cell->rect.w - r.x;
    }

    return (r);
}

//------------------------------------------------------------------------------------------
// Cell spans
//------------------------------------------------------------------------------------------

typedef struct bb_cell_span
{
    bb_cell* start;
    bb_cell* end;
} bb_cell_span;

void bb_cell_build_ancestor_array(oc_arena* arena, bb_cell* child, u32* outCount, bb_cell*** outCellPointerArray)
{
    //NOTE(martin): build an array including child and all its ancestors up to the root
    u32 ancestorCount = 0;
    for(bb_cell* cell = child;
        cell;
        cell = cell->parent)
    {
        ancestorCount++;
    }

    bb_cell** ancestors = oc_arena_push_array(arena, bb_cell*, ancestorCount);
    u32 ancestorIndex = 0;

    for(bb_cell* cell = child;
        cell;
        cell = cell->parent)
    {
        ancestors[ancestorIndex] = cell;
        ancestorIndex++;
    }
    *outCount = ancestorCount;
    *outCellPointerArray = ancestors;
}

bb_cell_span bb_cell_span_from_points(bb_point point, bb_point mark)
{
    /*NOTE:
		We want to select a span of sibling cells [S, E] where 1 edit point live directly to the left of S or in the subtree of S,
		and the other edit point lives directly right to E or in the subtree of E.

		- We build two lists from point and mark's parents up to the root
		- go downard in lockstep and detect when they diverge
		- this gives us a common ancestor (which can be one or both of the parents), so we have three cases:
			1) point and mark have the same parent (of course this can be detected early):
				-> S is the the right of the first edit point (in siblings order), E is to the left of the second edit point.
			2) One edit point (say P0) is in the common ancestor, the other (P1) is in the subtree of one of the common ancestor's children
				-> if M0 is to the left of the subtree in which P1 lies, S is to the right of M0 and E is the root of P1's subtree.
				   otherwise S is the root of P1's subtree and E is to the left of P0
			3) P0 and P1 are in two distinct subtrees T0 and T1 of the common ancestor
				-> S is the root of the first subtree, E is the root of the second subtree.
	*/
    oc_arena_scope scratch = oc_scratch_begin();
    bb_cell_span result = { 0 };

    if(point.parent == mark.parent)
    {
        //NOTE(martin): detect case 1) early
        oc_list_for(point.parent->children, child, bb_cell, parentElt)
        {
            if(child == point.leftFrom)
            {
                result = (bb_cell_span){ point.leftFrom, bb_point_left_cell(mark) };
            }
            else if(child == mark.leftFrom)
            {
                result = (bb_cell_span){ mark.leftFrom, bb_point_left_cell(point) };
            }
        }
    }
    else
    {

        //NOTE(martin): find common ancestor of point.parent and mark.parent
        bb_cell** pointAncestors = 0;
        u32 pointAncestorCount = 0;

        bb_cell** markAncestors = 0;
        u32 markAncestorCount = 0;

        bb_cell_build_ancestor_array(scratch.arena, point.parent, &pointAncestorCount, &pointAncestors);
        bb_cell_build_ancestor_array(scratch.arena, mark.parent, &markAncestorCount, &markAncestors);

        u32 minAncestorCount = oc_min(pointAncestorCount, markAncestorCount);
        bb_cell** pointIterator = pointAncestors + (pointAncestorCount - 1);
        bb_cell** markIterator = markAncestors + (markAncestorCount - 1);

        bb_cell* commonAncestor = *pointIterator;

        for(u32 i = 0; i < minAncestorCount; i++)
        {
            if(*pointIterator != *markIterator)
            {
                break;
            }
            commonAncestor = *pointIterator;

            if(i < pointAncestorCount - 1)
            {
                pointIterator--;
            }
            if(i < markAncestorCount - 1)
            {
                markIterator--;
            }
        }
        //NOTE(martin): here, commonAncestor is set to the last cell before the lineage diverge. pointIterator and markIterator
        //              point to the subtrees in which point and mark live. If point (resp. mark) is between children of the common ancestor,
        //              pointIterator (resp. markIterator) points to the common ancestor.

        if(point.parent == commonAncestor || mark.parent == commonAncestor)
        {
            //NOTE(martin): case 2)
            bb_point p0 = (point.parent == commonAncestor) ? point : mark;
            bb_cell* subTree = (point.parent == commonAncestor) ? *markIterator : *pointIterator;

            oc_list_for(commonAncestor->children, child, bb_cell, parentElt)
            {
                if(child == p0.leftFrom)
                {
                    result = (bb_cell_span){ p0.leftFrom, subTree };
                    break;
                }
                else if(child == subTree)
                {
                    result = (bb_cell_span){ subTree, bb_point_left_cell(p0) };
                    break;
                }
            }
        }
        else
        {
            //NOTE(martin): case 3)
            oc_list_for(commonAncestor->children, child, bb_cell, parentElt)
            {
                if(child == *pointIterator)
                {
                    result = (bb_cell_span){ *pointIterator, *markIterator };
                    break;
                }
                else if(child == *markIterator)
                {
                    result = (bb_cell_span){ *markIterator, *pointIterator };
                    break;
                }
            }
        }
    }
    oc_scratch_end(scratch);
    return result;
}

//------------------------------------------------------------------------------------
// cell box helpers
//------------------------------------------------------------------------------------
oc_rect bb_combined_box(oc_rect a, oc_rect b)
{
    f32 x0 = oc_min(a.x, b.x);
    f32 y0 = oc_min(a.y, b.y);
    f32 x1 = oc_max(a.x + a.w, b.x + b.w);
    f32 y1 = oc_max(a.y + a.h, b.y + b.h);
    return ((oc_rect){ x0, y0, x1 - x0, y1 - y0 });
}

oc_rect bb_cell_frame_box(bb_cell* cell)
{
    oc_rect r = cell->rect;
    /*    r.x += cell->frameOffset;
    r.w -= cell->frameOffset;
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
// cell insert/remove
//------------------------------------------------------------------------------------------

void bb_insert_at_cursor(bb_cell_editor* editor, bb_cell* cell)
{
    oc_vec2 start = bb_point_to_display_pos(editor, editor->cursor);
    cell->rect.x = start.x;
    cell->rect.y = start.y;

    bb_cell* cursorParent = editor->cursor.parent;

    if(bb_cell_has_children(cursorParent))
    {
        if(editor->cursor.leftFrom)
        {
            bb_cell_insert_before(editor->cursor.leftFrom, cell);
        }
        else
        {
            bb_cell_push(cursorParent, cell);
        }
    }
    else if(bb_cell_has_text(cursorParent))
    {
        if(cursorParent->kind == BB_CELL_HOLE)
        {
            bb_cell_insert(cursorParent, cell);
            bb_cell_recycle(editor, cursorParent);
        }
        else if(editor->cursor.offset == 0)
        {
            bb_cell_insert_before(cursorParent, cell);
        }
        else
        {
            bb_cell_insert(cursorParent, cell);
        }
    }
    else
    {
        bb_cell_insert(cursorParent, cell);
    }

    editor->cursor = (bb_point){ cell, 0, 0 };
    editor->mark = editor->cursor;

    //bb_mark_modified(editor, cell->parent);
}

void bb_insert_cell(bb_cell_editor* editor, bb_cell_kind kind)
{
    bb_cell* cell = bb_cell_alloc(editor, kind);
    bb_insert_at_cursor(editor, cell);
}

void bb_insert_hole(bb_cell_editor* editor)
{
    bb_cell* nextCell = 0;
    if(bb_cell_has_text(editor->cursor.parent))
    {
        nextCell = bb_cell_next_sibling(editor->cursor.parent);
    }
    else if(editor->cursor.leftFrom)
    {
        nextCell = editor->cursor.leftFrom;
    }

    if(nextCell && nextCell->kind == BB_CELL_HOLE)
    {
        editor->cursor = (bb_point){ .parent = nextCell };
        editor->mark = editor->cursor;
    }
    else
    {
        bb_insert_cell(editor, BB_CELL_HOLE);
    }
}

void bb_insert_list(bb_cell_editor* editor)
{
    bb_insert_cell(editor, BB_CELL_LIST);
}

void bb_insert_comment(bb_cell_editor* editor)
{
    bb_insert_cell(editor, BB_CELL_COMMENT);
}

void bb_insert_string_literal(bb_cell_editor* editor)
{
    bb_insert_cell(editor, BB_CELL_STRING);
}

//------------------------------------------------------------------------------------
// Lexing
//------------------------------------------------------------------------------------

#define BB_TOKEN_KEYWORDS(X) \
    X(KW_WHEN, "when")       \
    X(KW_CLAIM, "claim")     \
    X(KW_WISH, "wish")       \
    X(KW_SELF, "self")

enum
{

#define X(tok, str) OC_CAT2(BB_TOKEN_, tok),
    BB_TOKEN_KEYWORDS(X)
#undef X
};

typedef u32 bb_token;

typedef struct bb_lex_entry
{
    bb_token token;
    oc_str8 string;
} bb_lex_entry;

/*
const bb_lex_entry LEX_OPERATORS[] = {
#define X(tok, str) { .token = OC_CAT2(TOKEN_, tok), .string = mp_string_lit(str) },
    Q_TOKEN_OPERATORS(X)
#undef X
};
const u32 LEX_OPERATOR_COUNT = sizeof(LEX_OPERATORS) / sizeof(lex_entry);
*/
const bb_lex_entry BB_LEX_KEYWORDS[] = {
#define X(tok, str) { .token = OC_CAT2(BB_TOKEN_, tok), .string = OC_STR8_LIT(str) },
    BB_TOKEN_KEYWORDS(X)
#undef X
};
const u32 BB_LEX_KEYWORD_COUNT = sizeof(BB_LEX_KEYWORDS) / sizeof(bb_lex_entry);

typedef struct bb_lex_result
{
    bb_cell_kind kind;
    u64 valU64;
    f64 valF64;
    oc_str8 string;
} bb_lex_result;

/*
bb_lex_result bb_lex_operator(oc_str8 string, u64 byteOffset)
{
    u64 startOffset = byteOffset;
    u64 endOffset = byteOffset + 1;

    while(endOffset < string.len)
    {
        char c = string.ptr[endOffset];
        if(c == '+' || c == '-' || c == '*' || c == '/' || c == '%'
           || c == '!' || c == '=' || c == '<' || c == '>')
        {
            endOffset += 1;
        }
        else
        {
            break;
        }
    }
    bb_lex_result result = { .string = oc_str8_slice(string, startOffset, endOffset),
                             .kind = BB_CELL_SYMBOL };

    for(int i = 0; i < BB_LEX_OPERATOR_COUNT; i++)
    {
        if(!oc_str8_cmp(result.string, BB_LEX_OPERATORS[i].string))
        {
            result.valU64 = BB_LEX_OPERATORS[i].token;
            break;
        }
    }

    return (result);
}
*/
bb_lex_result bb_lex_placeholder(oc_str8 string, u64 byteOffset)
{
    u64 startOffset = byteOffset;
    u64 endOffset = byteOffset + 1;

    while(endOffset < string.len)
    {
        char c = string.ptr[endOffset];
        if((c >= 'a' && c <= 'z')
           || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9')
           || c == '_')
        {
            endOffset += 1;
        }
        else
        {
            break;
        }
    }
    bb_lex_result result = { .string = oc_str8_slice(string, startOffset, endOffset),
                             .kind = BB_CELL_PLACEHOLDER };
    return (result);
}

bb_lex_result bb_lex_identifier(oc_str8 string, u64 byteOffset)
{
    u64 startOffset = byteOffset;
    u64 endOffset = byteOffset + oc_utf8_size_from_leading_char(string.ptr[startOffset]);

    while(endOffset < string.len)
    {
        char c = string.ptr[endOffset];
        if((c >= 'a' && c <= 'z')
           || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9')
           || c == '_'
           || c == ':')
        {
            endOffset += 1;
        }
        else
        {
            break;
        }
    }
    bb_lex_result result = { .string = oc_str8_slice(string, startOffset, endOffset),
                             .kind = BB_CELL_SYMBOL };

    return (result);
}

bb_lex_result bb_lex_identifier_or_keyword(oc_str8 string, u64 byteOffset)
{
    bb_lex_result result = bb_lex_identifier(string, byteOffset);

    for(int i = 0; i < BB_LEX_KEYWORD_COUNT; i++)
    {
        if(!oc_str8_cmp(result.string, BB_LEX_KEYWORDS[i].string))
        {
            result.kind = BB_CELL_KEYWORD;
            result.valU64 = BB_LEX_KEYWORDS[i].token;
            break;
        }
    }

    return (result);
}

bb_lex_result bb_lex_number(oc_str8 string, u64 byteOffset)
{
    u64 startOffset = byteOffset;
    u64 endOffset = byteOffset;

    u64 numberU64 = 0;
    while(endOffset < string.len)
    {
        char c = string.ptr[endOffset];
        if(c >= '0' && c <= '9')
        {
            numberU64 *= 10;
            numberU64 += c - '0';
            endOffset += 1;
        }
        else
        {
            break;
        }
    }

    bb_lex_result result = {};

    f64 numberF64;
    if(endOffset < string.len
       && string.ptr[endOffset] == '.'
       && (endOffset + 1 >= string.len
           || string.ptr[endOffset + 1] != '.'))
    {
        endOffset += 1;

        u64 decimals = 0;
        u64 decimalCount = 0;

        while(endOffset < string.len)
        {
            char c = string.ptr[endOffset];
            if(c >= '0' && c <= '9')
            {
                decimals *= 10;
                decimals += c - '0';
                endOffset += 1;
                decimalCount += 1;
            }
            else
            {
                break;
            }
        }
        result.kind = BB_CELL_FLOAT;
        result.string = oc_str8_slice(string, startOffset, endOffset);
        result.valF64 = (f64)numberU64 + (f64)decimals / pow(10, decimalCount);
    }
    else
    {
        result.kind = BB_CELL_INT;
        result.string = oc_str8_slice(string, startOffset, endOffset);
        result.valU64 = numberU64;
    }
    return (result);
}

bb_lex_result bb_lex_error(oc_str8 string, u64 byteOffset)
{
    bb_lex_result result = { 0 };

    u64 startOffset = byteOffset;
    u64 endOffset = startOffset;

    while(endOffset < string.len)
    {
        oc_utf8_dec decode = oc_utf8_decode_at(string, endOffset);
        oc_utf32 c = decode.codepoint;

        if((c == '$')
           || (c >= 'a' && c <= 'z')
           || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9')
           || (c == '_')
           || (c == '+' || c == '-' || c == '*' || c == '/' || c == '%'
               || c == '!' || c == '=' || c == '<' || c == '>'))
        {
            goto end;
        }
        endOffset += decode.size;
    }
end:
    result = (bb_lex_result){ .string = oc_str8_slice(string, startOffset, endOffset),
                              .kind = BB_CELL_SYMBOL };
    return (result);
}

bb_lex_result bb_lex_next(oc_str8 string, u64 byteOffset, bb_cell_kind srcKind)
{
    bb_lex_result result = {};

    if(srcKind == BB_CELL_STRING
       || srcKind == BB_CELL_COMMENT)
    {
        //TODO for quote, set valU64?
        result.string = oc_str8_slice(string, byteOffset, string.len);
        result.kind = srcKind;
    }
    else if(byteOffset >= string.len)
    {
        //WARN: hole
        result.string = (oc_str8){ 0 };
        result.kind = BB_CELL_HOLE;
    }
    else
    {
        oc_utf8_dec decode = oc_utf8_decode_at(string, byteOffset);
        oc_utf32 c = decode.codepoint;

        if(c == '$')
        {
            result = bb_lex_placeholder(string, byteOffset);
        }
        else if((c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z')
                || c == '_')
        {
            result = bb_lex_identifier_or_keyword(string, byteOffset);
        }
        /*
        else if(c == '+' || c == '-' || c == '*' || c == '/' || c == '%'
                || c == '!' || c == '=' || c == '<' || c == '>')
        {
            result = bb_lex_operator(string, byteOffset);
        }
        */
        else if(c >= '0' && c <= '9')
        {
            result = bb_lex_number(string, byteOffset);
        }
        else
        {
            result = bb_lex_error(string, byteOffset);
        }
    }
    return (result);
}

void bb_relex_cell(bb_cell_editor* editor, bb_cell* cell, oc_str8 string)
{
    //	bb_mark_modified(editor, cell->parent);

    bb_cell_kind srcKind = cell->kind;
    bb_point cursorPoint = { .parent = cell, .leftFrom = 0, .offset = 0 };
    oc_vec2 cursorPos = bb_point_to_display_pos(editor, cursorPoint);
    bb_point nextPoint = editor->cursor;
    u32 byteOffset = 0;

    do
    {
        bb_lex_result lex = bb_lex_next(string, byteOffset, srcKind);
        byteOffset += lex.string.len;

        bb_cell_text_replace(editor, cell, lex.string);
        cell->kind = lex.kind;
        cell->valU64 = lex.valU64;
        cell->valF64 = lex.valF64;

        if(byteOffset < string.len)
        {
            bb_cell* prevCell = cell;
            cell = bb_cell_alloc(editor, BB_CELL_SYMBOL);
            bb_cell_insert(prevCell, cell);

            //NOTE: set default position and replace cursor if needed
            cell->rect.x = cursorPos.x + bb_display_offset_for_text_index(editor, string, byteOffset);
            cell->rect.y = cursorPos.y;

            if(editor->cursor.offset >= byteOffset)
            {
                nextPoint.parent = cell;
                nextPoint.offset = editor->cursor.offset - byteOffset;
            }
        }
    }
    while(byteOffset < string.len);

    editor->cursor = nextPoint;
    editor->mark = editor->cursor;
}

//------------------------------------------------------------------------------------
// Text edition
//------------------------------------------------------------------------------------

void bb_replace_text_selection_with_utf8(bb_cell_editor* editor, u64 count, char* input)
{
    bb_cell* cell = editor->cursor.parent;

    u32 selStart = oc_min(editor->cursor.offset, editor->mark.offset);
    u32 selEnd = oc_max(editor->cursor.offset, editor->mark.offset);

    if(cell->kind == BB_CELL_HOLE)
    {
        selStart = 0;
        selEnd = cell->text.len;
    }

    u64 newLen = cell->text.len + count - (selEnd - selStart);

    oc_arena_scope scratch = oc_scratch_begin();

    oc_str8_list insertList = {};
    oc_str8_list_push(scratch.arena, &insertList, oc_str8_slice(cell->text, 0, selStart));
    oc_str8_list_push(scratch.arena, &insertList, oc_str8_from_buffer(count, input));
    oc_str8_list_push(scratch.arena, &insertList, oc_str8_slice(cell->text, selEnd, cell->text.len));

    oc_str8 string = oc_str8_list_join(scratch.arena, insertList);
    editor->cursor.offset = selStart + count;

    editor->mark = editor->cursor;

    bb_relex_cell(editor, cell, string);
    //cell->kind = BB_CELL_SYMBOL;

    oc_scratch_end(scratch);
}

//------------------------------------------------------------------------------------
// Deletion
//------------------------------------------------------------------------------------

void bb_delete(bb_cell_editor* editor)
{
    if(!bb_point_same_cell(editor->cursor, editor->mark))
    {
        bb_cell_span span = bb_cell_span_from_points(editor->cursor, editor->mark);
        bb_cell* parent = span.start->parent;
        bb_cell* stop = bb_cell_next_sibling(span.end);

        bb_cell* cell = span.start;
        while(cell != stop)
        {
            bb_cell* nextCell = bb_cell_next_sibling(cell);
            bb_cell_recycle(editor, cell);
            cell = nextCell;
        }
        editor->cursor = (bb_point){ parent, stop, 0 };
        editor->mark = editor->cursor;

        //		bb_mark_modified(editor, parent);
    }
    else if(!editor->cursor.leftFrom && bb_cell_has_text(editor->cursor.parent))
    {
        bb_replace_text_selection_with_utf8(editor, 0, 0);
    }
}

//------------------------------------------------------------------------------------------
// Moves
//------------------------------------------------------------------------------------------
typedef enum
{
    BB_PREV,
    BB_NEXT
} bb_cursor_direction;

void bb_move_one(bb_cell_editor* editor, bb_cursor_direction direction)
{
    editor->cursor = (direction == BB_PREV)
                       ? bb_prev_point(editor->cursor)
                       : bb_next_point(editor->cursor);
}

void bb_move_vertical(bb_cell_editor* editor, bb_cursor_direction direction)
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

typedef void (*bb_move)(bb_cell_editor* editor, bb_cursor_direction direction);
typedef void (*bb_action)(bb_cell_editor* editor);

typedef struct bb_command
{
    oc_key_code key;
    oc_utf32 codePoint;
    oc_keymod_flags mods;

    bb_move move;
    bb_cursor_direction direction;
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
        .key = 58,
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
    /*
	{
		.codePoint = '[',
		.action = bb_insert_array,
		.rebuild = true,
		.updateCompletion = true,
		.focusCursor = true,
	},
	*/
    {
        .codePoint = ' ',
        .action = bb_insert_hole,
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
    /*
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
    */
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
    /*
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
        //NOTE(martin): we special case delete here, so that we don't move if we're deleting a selection
        if(command->action == bb_delete)
        {
            if(bb_point_equal(editor->cursor, editor->mark))
            {
                if(!bb_cell_has_text(editor->cursor.parent))
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
    i32 inlineCount;
    i32 alignedGroupCount;
    i32 alignedGroupSize;
    i32 indentedGroupSize;
    bool endGap;

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
                .alignedGroupCount = -1,
                .alignedGroupSize = 1,
            };
        }
        else if(!oc_list_empty(cell->children))
        {
            bb_cell* head = oc_list_first_entry(cell->children, bb_cell, parentElt);
            if(head->kind == BB_CELL_KEYWORD)
            {
                switch(head->valU64)
                {
                    case BB_TOKEN_KW_WHEN:
                        result = (cell_layout_options){
                            .vertical = true,
                            .inlineCount = 1,
                            .alignedGroupCount = 1,
                            .alignedGroupSize = 1,
                            .indentedGroupSize = 1,
                        };
                        break;
                    case BB_TOKEN_KW_CLAIM:
                        break;
                    case BB_TOKEN_KW_WISH:
                        break;
                }
            }
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
    bool endGap;

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
        result.rect.w += bb_cell_left_decorator_width(editor, cell);
        result.rect.w += bb_cell_right_decorator_width(editor, cell);

        result.lastLineWidth = result.rect.w;
        cell->lastLineWidth = result.rect.w;
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
            result.rect.w = 0;
            result.rect.h = editor->lineHeight;

            typedef enum
            {
                BB_LAYOUT_INLINE,
                BB_LAYOUT_ALIGNED,
                BB_LAYOUT_INDENTED
            } bb_layout_status;

            bb_layout_status status = BB_LAYOUT_INLINE;

            i32 groupSize = 0;
            i32 groupCount = 0;
            i32 maxGroupSize = options.inlineCount;

            f32 align = 0;
            f32 maxWidth = 0;
            f32 lineHeight = editor->lineHeight;

            childPos = (oc_vec2){ 0, 0 };
            childIndex = 0;

            oc_list_for(cell->children, child, bb_cell, parentElt)
            {
                //------------------------------------------------------------
                //NOTE(martin): count groups and switch between layout modes
                //------------------------------------------------------------
                if(childIndex)
                {
                    groupSize++;
                    if(status == BB_LAYOUT_INLINE)
                    {
                        align = childPos.x + editor->spaceWidth;
                    }
                }

                bool endOfLine = false;
                if(groupSize == maxGroupSize)
                {
                    groupSize = 0;
                    groupCount++;

                    //NOTE(martin): end of line is generated at the end of an aligned or indented group
                    endOfLine = (status == BB_LAYOUT_ALIGNED || status == BB_LAYOUT_INDENTED);

                    if(status == BB_LAYOUT_INLINE)
                    {
                        //NOTE(martin): at the end of the inline group, we switch to aligned layout
                        groupCount = 0;
                        maxGroupSize = options.alignedGroupSize;
                        status = BB_LAYOUT_ALIGNED;
                    }

                    if(status == BB_LAYOUT_ALIGNED && groupCount == options.alignedGroupCount)
                    {
                        //NOTE(martin): at the end of the aligned section, we switch to indented layout
                        groupCount = 0;
                        maxGroupSize = options.indentedGroupSize;
                        align = 2 * editor->spaceWidth;

                        //NOTE(martin): we can fallback here just after the end of the inline group, so
                        //              we need to force end of line in this case.
                        endOfLine = true;
                    }
                }

                if(endOfLine)
                {
                    //NOTE(martin): end of line
                    maxWidth = oc_max(maxWidth, childPos.x);
                    childPos.x = align;
                    childPos.y += lineHeight;
                    lineHeight = editor->lineHeight;

                    if(childIndex
                       && childResults[childIndex - 1].vertical
                       && childResults[childIndex - 1].endGap)
                    {
                        childPos.y += lineHeight;
                    }
                }
                else if(childIndex)
                {
                    //NOTE(martin): advance on the same line
                    childPos.x += editor->spaceWidth;
                }

                //------------------------------------------------------------------
                //NOTE(martin): set children relative coordinates and adjust widths
                //------------------------------------------------------------------
                child->rect.x = childPos.x;
                child->rect.y = childPos.y;

                childPos.x += childResults[childIndex].rect.w;
                lineHeight = oc_max(lineHeight, childResults[childIndex].rect.h);

                maxWidth = oc_max(maxWidth, childPos.x);

                childIndex++;
            }

            result.rect.w = maxWidth;
            result.rect.h = childPos.y + lineHeight;
            result.lastLineWidth = childPos.x;
        }

        //NOTE(martin): add width of parentheses / attribute marker
        f32 leftDecoratorW = bb_cell_left_decorator_width(editor, cell);
        f32 rightDecoratorW = bb_cell_right_decorator_width(editor, cell);

        if(result.lastLineWidth >= result.rect.w)
        {
            result.rect.w += rightDecoratorW;
        }
        result.rect.w += leftDecoratorW;

        result.lastLineWidth += leftDecoratorW + rightDecoratorW;

        oc_scratch_end(scratch);
    }
    cell->rect = result.rect;
    cell->lastLineWidth = result.lastLineWidth;

    return result;
}

void cell_update_rects(bb_cell_editor* editor, bb_cell* cell, oc_vec2 origin)
{
    cell->rect.x += origin.x;
    cell->rect.y += origin.y;

    oc_vec2 childOrigin = { cell->rect.x, cell->rect.y };
    childOrigin.x += bb_cell_left_decorator_width(editor, cell);

    oc_list_for(cell->children, child, bb_cell, parentElt)
    {
        cell_update_rects(editor, child, childOrigin);
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

    oc_str8 leftSep = { 0 };
    oc_str8 rightSep = { 0 };
    oc_set_color_rgba(1, 1, 1, 1);

    switch(cell->kind)
    {
        case BB_CELL_FLOAT:
        case BB_CELL_INT:
            oc_set_color_srgba(0.556, 0.716, 0.864, 1);
            break;

        case BB_CELL_KEYWORD:
            oc_set_color_rgba(0.797, 0.398, 0.359, 1);
            break;

        case BB_CELL_PLACEHOLDER:
            oc_set_color_rgba(1, 0.836, 0, 1);
            break;

        case BB_CELL_STRING:
            oc_set_color_srgba(0, 0.9, 0, 1);
            leftSep = OC_STR8("\"");
            rightSep = OC_STR8("\"");
            break;

        case BB_CELL_COMMENT:
            oc_set_color_srgba(0.94, 0.59, 0.21, 1);
            leftSep = OC_STR8("/*");
            rightSep = OC_STR8("*/");
            break;

        case BB_CELL_LIST:
            if(cell->parent)
            {
                leftSep = OC_STR8("(");
                rightSep = OC_STR8(")");
            }
            break;

        default:
            break;
    }

    oc_set_font(editor->font);
    oc_set_font_size(editor->fontSize);

    oc_vec2 pos = { box->rect.x, box->rect.y + editor->fontMetrics.ascent };
    if(leftSep.len)
    {
        oc_move_to(pos.x, pos.y);
        oc_text_outlines(leftSep);
        oc_fill();

        pos.x += oc_font_text_metrics(editor->font, editor->fontSize, leftSep).logical.w;
    }

    if(cell->text.len)
    {
        oc_set_font(editor->font);
        oc_set_font_size(editor->fontSize);

        oc_move_to(pos.x, pos.y);
        oc_text_outlines(cell->text);
        oc_fill();
    }

    if(rightSep.len)
    {
        f32 w = oc_font_text_metrics(editor->font, editor->fontSize, rightSep).logical.w;

        oc_move_to(box->rect.x + cell->lastLineWidth - w,
                   box->rect.y + box->rect.h - editor->lineHeight + editor->fontMetrics.ascent);
        oc_text_outlines(rightSep);
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
        //NOTE(martin): multiple nodes are selected, highlight the selection (in blue)
        bb_cell_span span = bb_cell_span_from_points(cursor, mark);
        bb_cell* parent = span.start->parent;
        bb_cell* stop = bb_cell_next_sibling(span.end);

        oc_rect startBox = bb_cell_frame_box(span.start);
        oc_rect box = startBox;
        oc_rect endBox = startBox;

        for(bb_cell* cell = span.start; cell != stop; cell = bb_cell_next_sibling(cell))
        {
            endBox = bb_cell_frame_box(cell);
            box = bb_combined_box(box, endBox);
        }

        oc_rect firstLine = {
            startBox.x,
            startBox.y,
            oc_max(0, box.x + box.w - startBox.x),
            lineHeight,
        };

        oc_rect innerSpan = {
            box.x,
            box.y + lineHeight,
            box.w,
            oc_max(0, box.h - 2 * lineHeight),
        };

        oc_rect lastLine = {
            box.x,
            box.y + box.h - lineHeight,
            oc_max(0, endBox.x + endBox.w - box.x),
            oc_max(0, endBox.y + endBox.h - (box.y + box.h - lineHeight)),
        };

        oc_set_color_rgba(0.2, 0.2, 1, 1);

        oc_rectangle_fill(firstLine.x, firstLine.y, firstLine.w, firstLine.h);
        oc_rectangle_fill(innerSpan.x, innerSpan.y, innerSpan.w, innerSpan.h);
        oc_rectangle_fill(lastLine.x, lastLine.y, lastLine.w, lastLine.h);

        bb_draw_cursor(editor);
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

            //  editor->animatedCursor = bb_point_to_display_pos(editor, editor->cursor);
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

void bb_card_draw_cells(oc_arena* frameArena, bb_cell_editor* editor, bb_card* card)
{
    oc_ui_box* box = oc_ui_container("cells", OC_UI_FLAG_DRAW_PROC)
    {
        if(card->root)
        {
            cell_update_layout(editor, card->root, (oc_vec2){ 10, 20 });
            cell_update_rects(editor, card->root, (oc_vec2){ 0 });
            build_cell_ui(frameArena, editor, card->root);
        }
    }

    if(editor->editedCard == card)
    {
        oc_ui_box_set_draw_proc(box, bb_editor_draw_proc, editor);
    }
    else
    {
        oc_ui_box_set_draw_proc(box, 0, editor);
    }
}

//------------------------------------------------------------------------------------------------
// Rule system
//------------------------------------------------------------------------------------------------

typedef enum
{
    BB_VALUE_SYMBOL,
    BB_VALUE_STRING,
    BB_VALUE_U64,
    BB_VALUE_F64,
    BB_VALUE_CARD_ID,

    BB_VALUE_LIST,

    BB_VALUE_PLACEHOLDER,

} bb_value_kind;

typedef struct bb_value
{
    oc_list_elt parentElt;
    oc_list children;

    bb_value_kind kind;

    oc_str8 string;
    u64 valU64;
    f64 valF64;

} bb_value;

typedef struct bb_fact
{
    oc_list_elt listElt;
    bb_value* root;
    u32 iteration;
} bb_fact;

typedef struct bb_facts_db bb_facts_db;

typedef void (*bb_listener_proc)(bb_value* match, oc_list bindings, bb_facts_db* factDb, oc_list cards);

typedef struct bb_listener
{
    oc_list_elt listElt;
    bb_value* pattern;
    bb_listener_proc proc;
    u32 lastRun;

} bb_listener;

typedef bb_fact* (*bb_responder_proc)(oc_arena* arena, bb_facts_db* factDb, bb_value* query, oc_list queryBindings, oc_list* factBindings);

typedef struct bb_responder
{
    oc_list_elt listElt;
    bb_value* pattern;
    bb_responder_proc proc;

} bb_responder;

typedef struct bb_facts_db
{
    u32 factCount;
    oc_list facts;

    oc_list cards;
    oc_list listeners;
    oc_list responders;

    u32 frame;
    u32 iteration;

} bb_facts_db;

oc_list bb_program_match_pattern_against_facts(oc_arena* arena, bb_facts_db* factDb, bb_value* pattern);

void bb_fact_db_push(oc_arena* arena, bb_facts_db* factDb, oc_list children)
{
    //NOTE: check if fact is already in db
    bb_value root = {
        .kind = BB_VALUE_LIST,
        .children = children,
    };
    oc_list matches = bb_program_match_pattern_against_facts(arena, factDb, &root);

    if(oc_list_empty(matches))
    {
        bb_fact* fact = oc_arena_push_type(arena, bb_fact);

        fact->iteration = factDb->iteration;

        fact->root = oc_arena_push_type(arena, bb_value);
        memset(fact->root, 0, sizeof(bb_value));
        fact->root->kind = BB_VALUE_LIST;
        fact->root->children = children;

        oc_list_push_back(&factDb->facts, &fact->listElt);
        factDb->factCount++;
    }
}

void bb_debug_print_value(bb_value* value)
{
    switch(value->kind)
    {
        case BB_VALUE_SYMBOL:
        case BB_VALUE_PLACEHOLDER:
            printf("%.*s", oc_str8_ip(value->string));
            break;

        case BB_VALUE_STRING:
            printf("\"%.*s\"", oc_str8_ip(value->string));
            break;

        case BB_VALUE_U64:
            printf("%llu", value->valU64);
            break;

        case BB_VALUE_F64:
            printf("%f", value->valF64);
            break;

        case BB_VALUE_CARD_ID:
            printf("card-%llu", value->valU64);
            break;

        case BB_VALUE_LIST:
        {
            printf("(");
            oc_list_for(value->children, child, bb_value, parentElt)
            {
                bb_debug_print_value(child);
                if(child->parentElt.next)
                {
                    printf(" ");
                }
            }
            printf(")");
        }
        break;
    }
}

void bb_debug_print_facts(bb_facts_db* factDb)
{
    printf("Facts:\n");
    i32 factIndex = 0;
    oc_list_for(factDb->facts, fact, bb_fact, listElt)
    {
        printf("\tfact #%i:\n\t\t", factIndex);
        bb_debug_print_value(fact->root);

        printf("\n");
        factIndex++;
    }
}

typedef struct bb_binding
{
    oc_list_elt listElt;
    oc_str8 name;
    bb_value* value;
} bb_binding;

bb_value* bb_find_binding(oc_list bindings, oc_str8 name)
{
    bb_value* value = 0;
    oc_list_for(bindings, binding, bb_binding, listElt)
    {
        if(!oc_str8_cmp(binding->name, name))
        {
            value = binding->value;
            break;
        }
    }
    return value;
}

bb_value* bb_program_eval_pattern(oc_arena* arena, bb_card* card, bb_cell* cell, oc_list bindings)
{
    bb_value* result = oc_arena_push_type(arena, bb_value);
    memset(result, 0, sizeof(bb_value));

    if(cell->kind == BB_CELL_LIST)
    {
        result->kind = BB_VALUE_LIST;
        oc_list_for(cell->children, childCell, bb_cell, parentElt)
        {
            bb_value* childVal = bb_program_eval_pattern(arena, card, childCell, bindings);
            oc_list_push_back(&result->children, &childVal->parentElt);
        }
    }
    else if(cell->kind == BB_CELL_KEYWORD && cell->valU64 == BB_TOKEN_KW_SELF)
    {
        result->kind = BB_VALUE_CARD_ID;
        result->valU64 = card->id;
    }
    else if(cell->kind == BB_CELL_FLOAT)
    {
        result->kind = BB_VALUE_F64;
        result->valF64 = cell->valF64;
    }
    else if(cell->kind == BB_CELL_INT)
    {
        result->kind = BB_VALUE_U64;
        result->valU64 = cell->valU64;
    }
    else if(cell->kind == BB_CELL_STRING)
    {
        result->kind = BB_VALUE_STRING;
        result->string = oc_str8_push_copy(arena, cell->text);
    }
    else if(cell->kind == BB_CELL_PLACEHOLDER)
    {
        result->kind = BB_VALUE_PLACEHOLDER;
        result->string = oc_str8_push_copy(arena, oc_str8_slice(cell->text, 1, cell->text.len));
    }
    else
    {
        bb_value* bound = bb_find_binding(bindings, cell->text);
        if(bound)
        {
            *result = *bound;
            result->parentElt = (oc_list_elt){ 0 };
        }
        else
        {
            result->kind = BB_VALUE_SYMBOL;
            result->string = oc_str8_push_copy(arena, cell->text);
        }
    }

    return (result);
}

bb_value* bb_program_match_pattern_against_value(oc_arena* arena, bb_value* value, bb_value* pattern, oc_list* bindings)
{
    bb_value* result = 0;
    bool match = (value->kind == pattern->kind || pattern->kind == BB_VALUE_PLACEHOLDER);

    if(match)
    {
        switch(pattern->kind)
        {
            case BB_VALUE_SYMBOL:
            case BB_VALUE_STRING:
                match = !oc_str8_cmp(value->string, pattern->string);
                break;

            case BB_VALUE_U64:
            case BB_VALUE_CARD_ID:
                match = (value->valU64 == pattern->valU64);
                break;

            case BB_VALUE_F64:
                match = (value->valF64 == pattern->valF64);
                break;

            case BB_VALUE_LIST:
            {
                bb_value* childA = oc_list_first_entry(value->children, bb_value, parentElt);
                bb_value* childB = oc_list_first_entry(pattern->children, bb_value, parentElt);
                for(;
                    childA != 0 && childB != 0;
                    childA = oc_list_next_entry(childA, bb_value, parentElt),
                    childB = oc_list_next_entry(childB, bb_value, parentElt))
                {
                    if(bb_program_match_pattern_against_value(arena, childA, childB, bindings) == 0)
                    {
                        match = false;
                        break;
                    }
                }
                if((childA == 0) != (childB == 0))
                {
                    match = false;
                }
            }
            break;

            case BB_VALUE_PLACEHOLDER:
            {
                bb_binding* binding = oc_arena_push_type(arena, bb_binding);
                binding->name = pattern->string;
                binding->value = value;
                oc_list_push_back(bindings, &binding->listElt);
            }
            break;
        }
        if(match)
        {
            result = value;
        }
    }

    return result;
}

typedef struct bb_match_result
{
    oc_list_elt listElt;
    bb_fact* fact;
    oc_list bindings;
} bb_match_result;

oc_list bb_program_match_pattern_against_facts(oc_arena* arena, bb_facts_db* factDb, bb_value* pattern)
{
    //NOTE: match pattern only against the facts in the db, not the builtin responders. This breaks a
    // recursion cycle where responders need to add facts to the db, which first tries to find if the facts
    // already exists, which would re-run the responders, etc...
    // Hence bb_fact_db_push() needs to match new facts only against the existing facts, not responders.

    oc_list results = { 0 };

    oc_list_for(factDb->facts, fact, bb_fact, listElt)
    {
        oc_list bindings = { 0 };
        bb_value* match = bb_program_match_pattern_against_value(arena, fact->root, pattern, &bindings);
        if(match)
        {
            bb_match_result* result = oc_arena_push_type(arena, bb_match_result);

            result->fact = fact;
            result->bindings = bindings;
            oc_list_push_back(&results, &result->listElt);
        }
    }
    return results;
}

oc_list bb_program_match_pattern(oc_arena* arena, bb_facts_db* factDb, bb_value* pattern)
{
    //NOTE: returns a list of match results. Each contain the matched value, and associated bindings
    oc_list results = { 0 };

    //NOTE: run the pattern against responders
    /*
        e.g. we have a query (when self points up at $x), we want it to match the responder ($p points $dir at $q)

        -> so we match the pattern of the _responder_ against the query, which returns the query with ($p = self, $dir = up, $q = $x),
        and call the responder routine with this. It should check which pages self points up to, and return
        (self points up at card-id) with ($x = card-id)
    */
    oc_list_for(factDb->responders, responder, bb_responder, listElt)
    {
        oc_list responderBindings = { 0 };
        bb_value* match = bb_program_match_pattern_against_value(arena, pattern, responder->pattern, &responderBindings);
        if(match)
        {
            oc_list answerBindings = { 0 };
            responder->proc(arena, factDb, match, responderBindings, &answerBindings);
            /*
            if(fact)
            {
                bb_match_result* result = oc_arena_push_type(arena, bb_match_result);
                result->fact = fact;
                result->bindings = answerBindings;
                oc_list_push_back(&results, &result->listElt);
            }
            */
        }
    }

    //NOTE: match againsts facts
    results = bb_program_match_pattern_against_facts(arena, factDb, pattern);

    return results;
}

void bb_program_interpret_cell(oc_arena* arena, bb_facts_db* factDb, bb_card* card, bb_cell* cell, oc_list bindings)
{
    if(cell->kind == BB_CELL_LIST && !oc_list_empty(cell->children))
    {
        bb_cell* head = oc_list_first_entry(cell->children, bb_cell, parentElt);

        if(head->kind == BB_CELL_KEYWORD)
        {
            if(head->valU64 == BB_TOKEN_KW_CLAIM)
            {
                oc_list list = { 0 };

                for(bb_cell* child = oc_list_next_entry(head, bb_cell, parentElt);
                    child != 0;
                    child = oc_list_next_entry(child, bb_cell, parentElt))
                {
                    bb_value* val = bb_program_eval_pattern(arena, card, child, bindings);
                    oc_list_push_back(&list, &val->parentElt);
                }
                bb_fact_db_push(arena, factDb, list);
            }
            else if(head->valU64 == BB_TOKEN_KW_WISH)
            {
                //NOTE: equivalent to  (claim self wishes ...)
                //TODO: this leaks the two created nodes if the fact was already in the db...
                oc_list list = { 0 };

                bb_value* self = oc_arena_push_type(arena, bb_value);
                memset(self, 0, sizeof(bb_value));
                self->kind = BB_VALUE_CARD_ID;
                self->valU64 = card->id;
                oc_list_push_back(&list, &self->parentElt);

                bb_value* wishes = oc_arena_push_type(arena, bb_value);
                memset(wishes, 0, sizeof(bb_value));
                wishes->kind = BB_VALUE_SYMBOL;
                wishes->string = oc_str8_push_cstring(arena, "wishes");
                oc_list_push_back(&list, &wishes->parentElt);

                for(bb_cell* child = oc_list_next_entry(head, bb_cell, parentElt);
                    child != 0;
                    child = oc_list_next_entry(child, bb_cell, parentElt))
                {
                    bb_value* val = bb_program_eval_pattern(arena, card, child, bindings);
                    oc_list_push_back(&list, &val->parentElt);
                }

                bb_fact_db_push(arena, factDb, list);
            }
            else if(head->valU64 == BB_TOKEN_KW_WHEN)
            {
                bb_cell* patternCell = oc_list_next_entry(head, bb_cell, parentElt);
                if(patternCell)
                {
                    if(cell->lastFrame != factDb->frame)
                    {
                        //NOTE: reset lastRun if this is the first time we encounter the cell this frame
                        cell->lastFrame = factDb->frame;
                        cell->lastRun = 0;
                    }

                    bb_value* pattern = bb_program_eval_pattern(arena, card, patternCell, bindings);
                    oc_list matches = bb_program_match_pattern(arena, factDb, pattern);

                    //TODO: should still execute but only for new matches... ie facts that are younger than the last iteration we ran

                    if(!oc_list_empty(matches))
                    {
                        oc_list_for(matches, match, bb_match_result, listElt)
                        {
                            if(match->fact->iteration > cell->lastRun)
                            {
                                //DEBUG
                                printf("matched fact: ");
                                bb_debug_print_value(match->fact->root);
                                printf("\n");

                                //TODO: interpret with bindings
                                for(bb_cell* child = oc_list_next_entry(patternCell, bb_cell, parentElt);
                                    child != 0;
                                    child = oc_list_next_entry(child, bb_cell, parentElt))
                                {
                                    bb_program_interpret_cell(arena, factDb, card, child, match->bindings);
                                }
                            }
                        }
                    }
                    cell->lastRun = factDb->iteration;
                }
            }
        }
    }
    factDb->iteration++;
}

void bb_builtin_listener_label(bb_value* match, oc_list bindings, bb_facts_db* factDb, oc_list cards)
{
    bb_value* p = bb_find_binding(bindings, OC_STR8("p"));
    bb_value* q = bb_find_binding(bindings, OC_STR8("q"));
    bb_value* s = bb_find_binding(bindings, OC_STR8("s"));

    if(q && s && q->kind == BB_VALUE_CARD_ID && s->kind == BB_VALUE_STRING)
    {
        oc_list_for(cards, card, bb_card, listElt)
        {
            if(card->id == q->valU64)
            {
                card->label = s->string;
                card->labelFrame = factDb->frame;
            }
        }
    }
}

typedef struct bb_color_entry
{
    oc_str8 string;
    oc_color color;
} bb_color_entry;

const bb_color_entry bb_highlight_colors[] = {
    {
        OC_STR8_LIT("red"),
        { 1, 0, 0, 1 },
    },
    {
        OC_STR8_LIT("green"),
        { 0, 1, 0, 1 },
    },
    {
        OC_STR8_LIT("blue"),
        { 0, 0, 1, 1 },
    },
};
const u32 bb_highlight_color_count = sizeof(bb_highlight_colors) / sizeof(bb_color_entry);

void bb_builtin_listener_highlight(bb_value* match, oc_list bindings, bb_facts_db* factDb, oc_list cards)
{
    bb_value* p = bb_find_binding(bindings, OC_STR8("p"));
    bb_value* q = bb_find_binding(bindings, OC_STR8("q"));
    bb_value* s = bb_find_binding(bindings, OC_STR8("s"));

    if(q && s && q->kind == BB_VALUE_CARD_ID && s->kind == BB_VALUE_STRING)
    {
        oc_color color = { 0 };
        bool found = false;
        for(u32 i = 0; i < bb_highlight_color_count; i++)
        {
            if(!oc_str8_cmp(s->string, bb_highlight_colors[i].string))
            {
                color = bb_highlight_colors[i].color;
                found = true;
                break;
            }
        }

        if(found)
        {
            oc_list_for(cards, card, bb_card, listElt)
            {
                if(card->id == q->valU64)
                {
                    card->highlight = color;
                    card->highlightFrame = factDb->frame;
                }
            }
        }
    }
}

void bb_program_init_builtin_listeners(oc_arena* arena, bb_facts_db* factDb)
{
    {
        bb_listener* listener = oc_arena_push_type(arena, bb_listener);

        bb_value* pattern = oc_arena_push_type(arena, bb_value);
        pattern->kind = BB_VALUE_LIST;

        bb_value* p = oc_arena_push_type(arena, bb_value);
        p->kind = BB_VALUE_PLACEHOLDER;
        p->string = oc_str8_push_cstring(arena, "p");
        oc_list_push_back(&pattern->children, &p->parentElt);

        bb_value* wishes = oc_arena_push_type(arena, bb_value);
        wishes->kind = BB_VALUE_SYMBOL;
        wishes->string = oc_str8_push_cstring(arena, "wishes");
        oc_list_push_back(&pattern->children, &wishes->parentElt);

        bb_value* q = oc_arena_push_type(arena, bb_value);
        q->kind = BB_VALUE_PLACEHOLDER;
        q->string = oc_str8_push_cstring(arena, "q");
        oc_list_push_back(&pattern->children, &q->parentElt);

        bb_value* is = oc_arena_push_type(arena, bb_value);
        is->kind = BB_VALUE_SYMBOL;
        is->string = oc_str8_push_cstring(arena, "is");
        oc_list_push_back(&pattern->children, &is->parentElt);

        bb_value* labeled = oc_arena_push_type(arena, bb_value);
        labeled->kind = BB_VALUE_SYMBOL;
        labeled->string = oc_str8_push_cstring(arena, "labeled");
        oc_list_push_back(&pattern->children, &labeled->parentElt);

        bb_value* s = oc_arena_push_type(arena, bb_value);
        s->kind = BB_VALUE_PLACEHOLDER;
        s->string = oc_str8_push_cstring(arena, "s");
        oc_list_push_back(&pattern->children, &s->parentElt);

        listener->pattern = pattern;
        listener->proc = bb_builtin_listener_label;

        oc_list_push_back(&factDb->listeners, &listener->listElt);
    }

    {
        bb_listener* listener = oc_arena_push_type(arena, bb_listener);

        bb_value* pattern = oc_arena_push_type(arena, bb_value);
        pattern->kind = BB_VALUE_LIST;

        bb_value* p = oc_arena_push_type(arena, bb_value);
        p->kind = BB_VALUE_PLACEHOLDER;
        p->string = oc_str8_push_cstring(arena, "p");
        oc_list_push_back(&pattern->children, &p->parentElt);

        bb_value* wishes = oc_arena_push_type(arena, bb_value);
        wishes->kind = BB_VALUE_SYMBOL;
        wishes->string = oc_str8_push_cstring(arena, "wishes");
        oc_list_push_back(&pattern->children, &wishes->parentElt);

        bb_value* q = oc_arena_push_type(arena, bb_value);
        q->kind = BB_VALUE_PLACEHOLDER;
        q->string = oc_str8_push_cstring(arena, "q");
        oc_list_push_back(&pattern->children, &q->parentElt);

        bb_value* is = oc_arena_push_type(arena, bb_value);
        is->kind = BB_VALUE_SYMBOL;
        is->string = oc_str8_push_cstring(arena, "is");
        oc_list_push_back(&pattern->children, &is->parentElt);

        bb_value* labeled = oc_arena_push_type(arena, bb_value);
        labeled->kind = BB_VALUE_SYMBOL;
        labeled->string = oc_str8_push_cstring(arena, "highlighted");
        oc_list_push_back(&pattern->children, &labeled->parentElt);

        bb_value* s = oc_arena_push_type(arena, bb_value);
        s->kind = BB_VALUE_PLACEHOLDER;
        s->string = oc_str8_push_cstring(arena, "s");
        oc_list_push_back(&pattern->children, &s->parentElt);

        listener->pattern = pattern;
        listener->proc = bb_builtin_listener_highlight;

        oc_list_push_back(&factDb->listeners, &listener->listElt);
    }
}

void bb_program_run_builtin_listeners(oc_arena* arena, bb_facts_db* factDb, oc_list cards)
{
    oc_list_for(factDb->listeners, listener, bb_listener, listElt)
    {
        oc_list matches = bb_program_match_pattern(arena, factDb, listener->pattern);

        oc_list_for(matches, match, bb_match_result, listElt)
        {
            if(match->fact->iteration > listener->lastRun)
            {
                listener->proc(match->fact->root, match->bindings, factDb, cards);
            }
        }
        listener->lastRun = factDb->iteration;
        factDb->iteration++;
    }
}

oc_str8 bb_direction_strings[] = {
    OC_STR8_LIT("up"),
    OC_STR8_LIT("left"),
    OC_STR8_LIT("down"),
    OC_STR8_LIT("right"),
};

bb_fact* bb_builtin_responder_point(oc_arena* arena, bb_facts_db* factDb, bb_value* query, oc_list queryBindings, oc_list* factBindings)
{
    bb_value* p = bb_find_binding(queryBindings, OC_STR8("p"));
    bb_value* dir = bb_find_binding(queryBindings, OC_STR8("dir"));
    bb_value* q = bb_find_binding(queryBindings, OC_STR8("q"));

    oc_list_for(factDb->cards, pointer, bb_card, listElt)
    {
        if(p->kind == BB_VALUE_PLACEHOLDER || (p->kind == BB_VALUE_CARD_ID && p->valU64 == pointer->id))
        {
            for(u32 dirIndex = 0; dirIndex < BB_WHISKER_DIRECTION_COUNT; dirIndex++)
            {
                if(dir->kind == BB_VALUE_PLACEHOLDER
                   || (dir->kind == BB_VALUE_SYMBOL && !oc_str8_cmp(dir->string, bb_direction_strings[dirIndex])))
                {
                    pointer->whiskerFrame[dirIndex] = factDb->frame;

                    oc_list_for(factDb->cards, pointee, bb_card, listElt)
                    {
                        if(q->kind == BB_VALUE_PLACEHOLDER || (q->kind == BB_VALUE_CARD_ID && q->valU64 == pointee->id))
                        {
                            oc_vec2 pCenter = {
                                pointer->rect.x + pointer->rect.w / 2,
                                pointer->rect.y + pointer->rect.h / 2,
                            };
                            oc_rect pRect = pointer->rect;
                            oc_rect qRect = pointee->rect;

                            bool test = false;
                            switch(dirIndex)
                            {
                                case BB_WHISKER_DIRECTION_UP:
                                    test = pCenter.x >= qRect.x
                                        && pCenter.x <= (qRect.x + qRect.w)
                                        && pRect.y - BB_WHISKER_SIZE >= qRect.y
                                        && pRect.y - BB_WHISKER_SIZE <= (qRect.y + qRect.h);
                                    break;
                                case BB_WHISKER_DIRECTION_LEFT:
                                    test = pCenter.y >= qRect.y
                                        && pCenter.y <= (qRect.y + qRect.h)
                                        && pRect.x - BB_WHISKER_SIZE >= qRect.x
                                        && pRect.x - BB_WHISKER_SIZE <= (qRect.x + qRect.w);
                                    break;
                                case BB_WHISKER_DIRECTION_DOWN:
                                    test = pCenter.x >= qRect.x
                                        && pCenter.x <= (qRect.x + qRect.w)
                                        && pRect.y + pRect.h + BB_WHISKER_SIZE >= qRect.y
                                        && pRect.y + pRect.h + BB_WHISKER_SIZE <= (qRect.y + qRect.h);
                                    break;
                                case BB_WHISKER_DIRECTION_RIGHT:
                                    test = pCenter.y >= qRect.y
                                        && pCenter.y <= (qRect.y + qRect.h)
                                        && pRect.x + pRect.w + BB_WHISKER_SIZE >= qRect.x
                                        && pRect.x + pRect.w + BB_WHISKER_SIZE <= (qRect.x + qRect.w);
                                    break;
                            }

                            if(test)
                            {
                                pointer->whiskerBoldFrame[dirIndex] = factDb->frame;

                                //NOTE add a fact to the database, which will be picked up next iteration...
                                oc_list list = { 0 };

                                bb_value* pVal = oc_arena_push_type(arena, bb_value);
                                pVal->kind = BB_VALUE_CARD_ID;
                                pVal->valU64 = pointer->id;
                                oc_list_push_back(&list, &pVal->parentElt);

                                bb_value* pointsVal = oc_arena_push_type(arena, bb_value);
                                pointsVal->kind = BB_VALUE_SYMBOL;
                                pointsVal->string = OC_STR8("points");
                                oc_list_push_back(&list, &pointsVal->parentElt);

                                bb_value* dirVal = oc_arena_push_type(arena, bb_value);
                                dirVal->kind = BB_VALUE_SYMBOL;
                                dirVal->string = bb_direction_strings[dirIndex];
                                oc_list_push_back(&list, &dirVal->parentElt);

                                bb_value* atVal = oc_arena_push_type(arena, bb_value);
                                atVal->kind = BB_VALUE_SYMBOL;
                                atVal->string = OC_STR8("at");
                                oc_list_push_back(&list, &atVal->parentElt);

                                bb_value* qVal = oc_arena_push_type(arena, bb_value);
                                qVal->kind = BB_VALUE_CARD_ID;
                                qVal->valU64 = pointee->id;
                                oc_list_push_back(&list, &qVal->parentElt);

                                bb_fact_db_push(arena, factDb, list);
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}

void bb_program_init_builtin_responders(oc_arena* arena, bb_facts_db* factDb)
{
    {
        bb_responder* responder = oc_arena_push_type(arena, bb_responder);

        bb_value* pattern = oc_arena_push_type(arena, bb_value);
        pattern->kind = BB_VALUE_LIST;

        bb_value* p = oc_arena_push_type(arena, bb_value);
        p->kind = BB_VALUE_PLACEHOLDER;
        p->string = oc_str8_push_cstring(arena, "p");
        oc_list_push_back(&pattern->children, &p->parentElt);

        bb_value* points = oc_arena_push_type(arena, bb_value);
        points->kind = BB_VALUE_SYMBOL;
        points->string = oc_str8_push_cstring(arena, "points");
        oc_list_push_back(&pattern->children, &points->parentElt);

        bb_value* direction = oc_arena_push_type(arena, bb_value);
        direction->kind = BB_VALUE_PLACEHOLDER;
        direction->string = oc_str8_push_cstring(arena, "dir");
        oc_list_push_back(&pattern->children, &direction->parentElt);

        bb_value* at = oc_arena_push_type(arena, bb_value);
        at->kind = BB_VALUE_SYMBOL;
        at->string = oc_str8_push_cstring(arena, "at");
        oc_list_push_back(&pattern->children, &at->parentElt);

        bb_value* q = oc_arena_push_type(arena, bb_value);
        q->kind = BB_VALUE_PLACEHOLDER;
        q->string = oc_str8_push_cstring(arena, "q");
        oc_list_push_back(&pattern->children, &q->parentElt);

        responder->pattern = pattern;
        responder->proc = bb_builtin_responder_point;

        oc_list_push_back(&factDb->responders, &responder->listElt);
    }
}

void bb_program_update(bb_facts_db* factDb, oc_list cards)
{
    factDb->facts = (oc_list){ 0 };
    factDb->factCount = 0;
    factDb->iteration = 1;
    factDb->cards = cards;

    oc_arena_scope scratch = oc_scratch_begin();

    //NOTE: reset built-in listeners last run
    oc_list_for(factDb->listeners, listener, bb_listener, listElt)
    {
        listener->lastRun = 0;
    }

    //NOTE: run until fixed point (i.e. until iteration doesn't generate any new facts)
    u32 prevFactCount = 0;
    do
    {
        prevFactCount = factDb->factCount;

        oc_list_for(cards, card, bb_card, listElt)
        {
            oc_list_for(card->root->children, cell, bb_cell, parentElt)
            {
                bb_program_interpret_cell(scratch.arena, factDb, card, cell, (oc_list){ 0 });
            }
        }

        bb_program_run_builtin_listeners(scratch.arena, factDb, cards);
    }
    while(prevFactCount != factDb->factCount);

    bb_debug_print_facts(factDb);

    oc_scratch_end(scratch);

    factDb->frame++;
}

//------------------------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------------------------

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

typedef struct bb_card_draw_proc_data
{
    bb_cell_editor* editor;
    bb_card* card;
    u32 frame;
} bb_card_draw_proc_data;

void bb_card_draw_proc(oc_ui_box* box, void* user)
{
    bb_card_draw_proc_data* data = (bb_card_draw_proc_data*)user;

    const f32 fontSize = 42;
    if(data->card->labelFrame == data->frame - 1)
    {
        oc_font_metrics fontMetrics = oc_font_get_metrics(data->editor->font, fontSize);
        oc_text_metrics metrics = oc_font_text_metrics(data->editor->font, fontSize, data->card->label);
        f32 x = data->card->rect.x + (data->card->rect.w - metrics.logical.w) / 2;
        f32 y = data->card->rect.y + (data->card->rect.h - metrics.logical.h) / 2 + fontMetrics.ascent;

        oc_move_to(x, y);
        oc_set_font(data->editor->font);
        oc_set_font_size(fontSize);
        oc_set_color_rgba(1, 1, 1, 0.5);
        oc_text_outlines(data->card->label);
        oc_fill();
    }
    if(data->card->highlightFrame == data->frame - 1)
    {
        oc_color color = data->card->highlight;
        color.a = 0.3;
        oc_set_color(color);
        oc_rounded_rectangle_fill(data->card->rect.x - 10,
                                  data->card->rect.y - 10,
                                  data->card->rect.w + 20,
                                  data->card->rect.h + 20,
                                  5 + 10);
    }
    for(u32 i = 0; i < BB_WHISKER_DIRECTION_COUNT; i++)
    {
        if(data->card->whiskerFrame[i] == data->frame - 1)
        {
            oc_rect rect = data->card->rect;

            oc_set_color_rgba(0, 1, 0, 1);

            if(data->card->whiskerBoldFrame[i] == data->frame - 1)
            {
                oc_set_width(2);
            }
            else
            {
                oc_set_width(1);
            }

            switch(i)
            {
                case BB_WHISKER_DIRECTION_UP:
                    oc_move_to(rect.x + 0.2 * rect.w, rect.y - 5);
                    oc_line_to(rect.x + 0.8 * rect.w, rect.y - 5);
                    oc_stroke();
                    oc_move_to(rect.x + rect.w / 2, rect.y - 5);
                    oc_line_to(rect.x + rect.w / 2, rect.y - BB_WHISKER_SIZE + 5);
                    oc_stroke();
                    oc_circle_stroke(rect.x + rect.w / 2, rect.y - BB_WHISKER_SIZE, 5);
                    break;
                case BB_WHISKER_DIRECTION_LEFT:
                    oc_move_to(rect.x - 5, rect.y + 0.2 * rect.h);
                    oc_line_to(rect.x - 5, rect.y + 0.8 * rect.h);
                    oc_stroke();
                    oc_move_to(rect.x - 5, rect.y + rect.h / 2);
                    oc_line_to(rect.x - BB_WHISKER_SIZE + 5, rect.y + rect.h / 2);
                    oc_stroke();
                    oc_circle_stroke(rect.x - BB_WHISKER_SIZE, rect.y + rect.h / 2, 5);
                    break;
                case BB_WHISKER_DIRECTION_DOWN:
                    oc_move_to(rect.x + 0.2 * rect.w, rect.y + rect.h + 5);
                    oc_line_to(rect.x + 0.8 * rect.w, rect.y + rect.h + 5);
                    oc_stroke();
                    oc_move_to(rect.x + rect.w / 2, rect.y + rect.h + 5);
                    oc_line_to(rect.x + rect.w / 2, rect.y + rect.h + BB_WHISKER_SIZE - 5);
                    oc_stroke();
                    oc_circle_stroke(rect.x + rect.w / 2, rect.y + rect.h + BB_WHISKER_SIZE, 5);
                    break;
                case BB_WHISKER_DIRECTION_RIGHT:
                    oc_move_to(rect.x + rect.w + 5, rect.y + 0.2 * rect.h);
                    oc_line_to(rect.x + rect.w + 5, rect.y + 0.8 * rect.h);
                    oc_stroke();
                    oc_move_to(rect.x + rect.w + 5, rect.y + rect.h / 2);
                    oc_line_to(rect.x + rect.w + BB_WHISKER_SIZE - 5, rect.y + rect.h / 2);
                    oc_stroke();
                    oc_circle_stroke(rect.x + rect.w + BB_WHISKER_SIZE, rect.y + rect.h / 2, 5);
                    break;
                default:
                    break;
            }
        }
    }
}

int main()
{
    oc_init();

    oc_rect windowRect = { .x = 100, .y = 100, .w = 1200, .h = 800 };
    oc_window window = oc_window_create(windowRect, OC_STR8("Babbler"), 0);

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

    oc_list InactiveList = { 0 };
    oc_list backgroundList = { 0 };
    oc_list activeList = { 0 };

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
            .rect = { 500, 400, 400, 200 },
        },
    };

    for(int i = 0; i < 8; i++)
    {
        cards[i].displayRect = cards[i].rect;
    }

    oc_list_push_back(&InactiveList, &cards[0].listElt);
    oc_list_push_back(&InactiveList, &cards[1].listElt);
    oc_list_push_back(&InactiveList, &cards[2].listElt);

    oc_list_push_back(&backgroundList, &cards[3].listElt);
    oc_list_push_back(&backgroundList, &cards[4].listElt);

    oc_list_push_back(&activeList, &cards[5].listElt);
    oc_list_push_back(&activeList, &cards[6].listElt);
    oc_list_push_back(&activeList, &cards[7].listElt);

    f32 cardAnimationTimeConstant = 0.2;

    oc_font_metrics metrics = oc_font_get_metrics(font, 14);
    oc_text_metrics spaceMetrics = oc_font_text_metrics(font, 14, OC_STR8(" "));

    bb_cell_editor editor = {
        .font = font,
        .fontSize = 14,
        .fontMetrics = metrics,
        .lineHeight = metrics.ascent + metrics.descent + metrics.lineGap,
        .spaceWidth = spaceMetrics.logical.w,
        .nextCellId = 100,
    };

    oc_arena_init(&editor.arena);

    for(int i = 0; i < 8; i++)
    {
        cards[i].root = oc_arena_push_type(&editor.arena, bb_cell);
        memset(cards[i].root, 0, sizeof(bb_cell));
        cards[i].root->id = 0;
        cards[i].root->kind = BB_CELL_LIST;
    }
    /*
    bb_cell* root = oc_arena_push_type(&editor.arena, bb_cell);
    memset(root, 0, sizeof(bb_cell));
    root->id = 0;
    root->kind = BB_CELL_LIST;

    bb_cell* whenList = oc_arena_push_type(&editor.arena, bb_cell);
    memset(whenList, 0, sizeof(bb_cell));
    whenList->id = 1;
    whenList->parent = root;
    oc_list_push_back(&root->children, &whenList->parentElt);
    root->childCount++;
    whenList->kind = BB_CELL_LIST;

    bb_cell* when = oc_arena_push_type(&editor.arena, bb_cell);
    memset(when, 0, sizeof(bb_cell));
    when->id = 2;
    when->parent = whenList;
    oc_list_push_back(&whenList->children, &when->parentElt);
    whenList->childCount++;
    when->kind = BB_CELL_SYMBOL;
    when->text = OC_STR8("when");
    bb_relex_cell(&editor, when, OC_STR8("when"));

    bb_cell* claimList = oc_arena_push_type(&editor.arena, bb_cell);
    memset(claimList, 0, sizeof(bb_cell));
    claimList->id = 3;
    claimList->parent = root;
    oc_list_push_back(&root->children, &claimList->parentElt);
    root->childCount++;
    claimList->kind = BB_CELL_LIST;

    bb_cell* claim = oc_arena_push_type(&editor.arena, bb_cell);
    memset(claim, 0, sizeof(bb_cell));
    claim->id = 4;
    claim->parent = claimList;
    oc_list_push_back(&claimList->children, &claim->parentElt);
    claimList->childCount++;
    claim->kind = BB_CELL_SYMBOL;
    claim->text = OC_STR8("claim");
    bb_relex_cell(&editor, claim, OC_STR8("claim"));

    bb_cell* self = oc_arena_push_type(&editor.arena, bb_cell);
    memset(self, 0, sizeof(bb_cell));
    self->id = 5;
    self->parent = claimList;
    oc_list_push_back(&claimList->children, &self->parentElt);
    claimList->childCount++;
    self->kind = BB_CELL_SYMBOL;
    self->text = OC_STR8("self");
    bb_relex_cell(&editor, self, OC_STR8("self"));

    cards[7].root = root;

    editor.cursor = (bb_point){
        .parent = self,
        .leftFrom = 0,
        .offset = 3,
    },
    editor.mark = editor.cursor;
    */

    bb_facts_db factDb = { .frame = 2 };

    bb_program_init_builtin_listeners(&editor.arena, &factDb);
    bb_program_init_builtin_responders(&editor.arena, &factDb);

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

        //NOTE(martin): update program
        bb_program_update(&factDb, activeList);

        //NOTE(martin): handle keyboard shortcuts
        if(editor.editedCard)
        {
            bool runCommand = false;
            oc_keymod_flags mods = oc_key_mods(&ui.input) & (~OC_KEYMOD_MAIN_MODIFIER);

            for(int i = 0; i < BB_COMMAND_COUNT; i++)
            {
                const bb_command* command = &(BB_COMMANDS[i]);

                if((oc_key_press_count(&ui.input, command->key) || oc_key_repeat_count(&ui.input, command->key))
                   && command->mods == mods)
                {
                    bb_run_command(&editor, command);
                    runCommand = true;
                    break;
                }
            }

            if(!runCommand)
            {
                //NOTE(martin): handle character input
                oc_str32 textInput = oc_input_text_utf32(scratch.arena, &ui.input);

                for(int textIndex = 0; textIndex < textInput.len; textIndex++)
                {
                    oc_utf32 codePoint = textInput.ptr[textIndex];

                    //NOTE: character-based commands
                    if(editor.cursor.parent->kind != BB_CELL_STRING
                       && editor.cursor.parent->kind != BB_CELL_CHAR
                       && editor.cursor.parent->kind != BB_CELL_COMMENT)
                    {
                        bool found = false;
                        for(int i = 0; i < BB_COMMAND_COUNT; i++)
                        {
                            const bb_command* command = &(BB_COMMANDS[i]);

                            if(command->codePoint == codePoint)
                            {
                                bb_run_command(&editor, command);
                                found = true;
                                break;
                            }
                        }
                        if(found)
                        {
                            continue;
                        }
                    }

                    //TODO: allow replacing cell span with text

                    //NOTE: text insertion
                    if(!bb_cell_has_text(editor.cursor.parent))
                    {
                        bb_insert_hole(&editor);
                    }

                    if(editor.cursor.parent == editor.mark.parent)
                    {
                        char backing[4];
                        oc_str8 utf8Input = oc_utf8_encode(backing, codePoint);
                        bb_replace_text_selection_with_utf8(&editor, utf8Input.len, utf8Input.ptr);
                    }

                    // bb_rebuild(editor);
                }

                if(textInput.len)
                {
                    //  bb_reset_cursor_blink(editor);
                }
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

                bool selectedEdit = false;

                oc_ui_box* canvas = oc_ui_container("center-panel", OC_UI_FLAG_DRAW_BORDER | OC_UI_FLAG_CLICKABLE)
                {
                    oc_ui_sig canvasSig = oc_ui_box_sig(canvas);
                    if(canvasSig.dragging)
                    {
                        canvas->scroll.x -= canvasSig.delta.x;
                        canvas->scroll.y -= canvasSig.delta.y;
                    }

                    oc_ui_container("contents", OC_UI_FLAG_CLICKABLE)
                    {
                        oc_list_for(activeList, card, bb_card, listElt)
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
                                if(sig.rightPressed)
                                {
                                    selectedEdit = true;
                                    editor.editedCard = card;
                                    editor.cursor = (bb_point){
                                        .parent = card->root,
                                        .leftFrom = oc_list_first_entry(card->root->children, bb_cell, parentElt),
                                    };
                                    editor.mark = editor.cursor;
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
                                                     .layout.margin = {
                                                         .x = 10,
                                                         .y = 10,
                                                     },
                                                 },
                                                 OC_UI_STYLE_SIZE | OC_UI_STYLE_FLOAT | OC_UI_STYLE_BG_COLOR | OC_UI_STYLE_BORDER_COLOR | OC_UI_STYLE_BORDER_SIZE | OC_UI_STYLE_ROUNDNESS | OC_UI_STYLE_LAYOUT_MARGINS);

                                oc_ui_container_str8(key,
                                                     OC_UI_FLAG_CLIP
                                                         | OC_UI_FLAG_DRAW_BACKGROUND
                                                         | OC_UI_FLAG_DRAW_BORDER
                                                         | OC_UI_FLAG_CLICKABLE
                                                         | OC_UI_FLAG_BLOCK_MOUSE
                                                         | OC_UI_FLAG_DRAW_PROC)
                                {
                                    oc_ui_label_str8(key);
                                    bb_card_draw_cells(scratch.arena, &editor, card);
                                }

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
                                                 },
                                                 OC_UI_STYLE_SIZE | OC_UI_STYLE_FLOAT);

                                oc_str8 illumKey = oc_str8_pushf(scratch.arena, "illum-%llu", card->id);
                                oc_ui_box* box = oc_ui_box_make_str8(illumKey, OC_UI_FLAG_DRAW_PROC);

                                bb_card_draw_proc_data* data = oc_arena_push_type(scratch.arena, bb_card_draw_proc_data);
                                data->card = card;
                                data->frame = factDb.frame;
                                data->editor = &editor;

                                oc_ui_box_set_draw_proc(box, bb_card_draw_proc, data);
                            }
                        }
                    }
                }
                if(oc_ui_box_sig(canvas).rightPressed && !selectedEdit)
                {
                    editor.editedCard = 0;
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
                            oc_list_for_safe(InactiveList, card, bb_card, listElt)
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
                                                     .layout.margin = {
                                                         .x = 10,
                                                         .y = 10,
                                                     },
                                                 },
                                                 OC_UI_STYLE_SIZE | OC_UI_STYLE_FLOAT | OC_UI_STYLE_BG_COLOR | OC_UI_STYLE_BORDER_COLOR | OC_UI_STYLE_BORDER_SIZE | OC_UI_STYLE_ROUNDNESS | OC_UI_STYLE_LAYOUT_MARGINS);

                                oc_ui_box* box = oc_ui_container_str8(key, OC_UI_FLAG_CLIP | OC_UI_FLAG_DRAW_BACKGROUND | OC_UI_FLAG_CLICKABLE)
                                {
                                    oc_ui_label_str8(key);
                                    bb_card_draw_cells(scratch.arena, &editor, card);
                                }

                                oc_ui_sig sig = oc_ui_box_sig(box);
                                if(sig.pressed)
                                {
                                    oc_vec2 mousePos = oc_mouse_position(&ui.input);

                                    card->rect.x = mousePos.x - sig.mouse.x;
                                    card->rect.y = mousePos.y - sig.mouse.y;
                                    oc_list_remove(&InactiveList, &card->listElt);
                                    oc_list_push_back(&activeList, &card->listElt);

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
                                         .layout.margin = {
                                             .x = 10,
                                             .y = 10,
                                         },
                                     },
                                     OC_UI_STYLE_SIZE | OC_UI_STYLE_FLOAT | OC_UI_STYLE_BG_COLOR | OC_UI_STYLE_BORDER_COLOR | OC_UI_STYLE_BORDER_SIZE | OC_UI_STYLE_ROUNDNESS | OC_UI_STYLE_LAYOUT_MARGINS);

                    oc_str8 key = oc_str8_pushf(scratch.arena, "card-%u", dragging->id);

                    oc_ui_container_str8(key,
                                         OC_UI_FLAG_CLIP
                                             | OC_UI_FLAG_DRAW_BACKGROUND
                                             | OC_UI_FLAG_DRAW_BORDER
                                             | OC_UI_FLAG_CLICKABLE
                                             | OC_UI_FLAG_BLOCK_MOUSE
                                             | (dragging ? OC_UI_FLAG_OVERLAY : 0))
                    {
                        oc_ui_label_str8(key);
                        bb_card_draw_cells(scratch.arena, &editor, dragging);
                    }
                }

                if(dragging && oc_mouse_released(&ui.input, OC_MOUSE_LEFT))
                {
                    if(cardHoveringLeftPanel)
                    {
                        dragging->rect.x += leftPanelScroll->scroll.x;
                        dragging->rect.y += leftPanelScroll->scroll.y;

                        oc_list_remove(&activeList, &dragging->listElt);
                        if(!insertBefore)
                        {
                            oc_list_push_back(&InactiveList, &dragging->listElt);
                        }
                        else
                        {
                            oc_list_insert_before(&InactiveList, insertBefore, &dragging->listElt);
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

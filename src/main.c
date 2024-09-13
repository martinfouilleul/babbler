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

typedef struct bb_card
{
    oc_list_elt listElt;

    u32 id;
    oc_rect rect;
    oc_rect displayRect;
} bb_card;

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

                                card->displayRect.x += 0.1 * (x - card->displayRect.x);
                                card->displayRect.y += 0.1 * (y - card->displayRect.y);
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
                        dragging->displayRect.w += 0.1 * (100 - dragging->displayRect.w);
                        dragging->displayRect.h += 0.1 * (100 - dragging->displayRect.h);
                    }
                    else
                    {
                        dragging->displayRect.w += 0.1 * (dragging->rect.w - dragging->displayRect.w);
                        dragging->displayRect.h += 0.1 * (dragging->rect.h - dragging->displayRect.h);
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

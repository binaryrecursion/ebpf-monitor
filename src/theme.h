#ifndef THEME_H
#define THEME_H

#include <stdint.h>

#define rgb(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))



typedef struct {

    uint32_t bg_panel;       
    uint32_t bg_header_row;  
    uint32_t bg_alt_row;    
    uint32_t bg_selected;   
    uint32_t bg_anom_row;    
    
    uint32_t fg_primary;     
    uint32_t fg_secondary;  
    uint32_t fg_dim;         

    uint32_t fg_green;
    uint32_t fg_yellow;
    uint32_t fg_orange;
    uint32_t fg_red;
    uint32_t fg_cyan;
    uint32_t fg_blue;
    uint32_t fg_magenta;
    uint32_t fg_teal;
    uint32_t fg_purple;

    
    uint32_t border_dim;     
    uint32_t border_title;  
  
    const char *bh;         
    const char *bv;         
    const char *btl;         
    const char *btr;        
    const char *bbl;         
    const char *bbr;         
    const char *bml;         
    const char *bmr;         
    const char *bdh;        
    const char *bdtl;       
    const char *bdtr;        
    const char *bdbl;       
    const char *bdbr;        
    const char *bdv;
} theme_t;


extern const theme_t THEME_DARK;    
extern const theme_t THEME_MOCHA;   
extern const theme_t *T;            
void theme_set(const theme_t *t);

#endif /* THEME_H */

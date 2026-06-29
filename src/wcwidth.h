#ifndef ZGT_WCWIDTH_H
#define ZGT_WCWIDTH_H

// Column width of a Unicode code point as a terminal would lay it out:
//   0 = zero-width (combining marks, control chars, format chars)
//   1 = normal
//   2 = wide (CJK ideographs, fullwidth forms, most emoji)
// Based on Markus Kuhn's public-domain mk_wcwidth(), plus emoji ranges.
int zgt_char_width(char32_t c);

#endif // ZGT_WCWIDTH_H

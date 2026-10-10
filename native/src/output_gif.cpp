#include "output_gif.hpp"

#include <sstream>

std::unique_ptr<FfmpegEncoder> openGifEncoder(const std::string& outputPath, int width, int height,
                                              float fps, const std::string& ffmpegPath)
{
    // Formatted exactly as the old shell command line streamed them.
    std::ostringstream size;
    size << width << "x" << height;
    std::ostringstream rate;
    rate << fps;

    // Use ffmpeg with palettegen filter for high-quality GIF output
    // Two-pass approach via complex filtergraph for best palette.
    // The filtergraph is one argv entry; it used to be shell-quoted.
    std::vector<std::string> args = {
        "-y",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgba",
        "-s",
        size.str(),
        "-r",
        rate.str(),
        "-i",
        "pipe:0",
        "-filter_complex",
        "[0:v]split[a][b];[a]palettegen=max_colors=256:stats_mode=diff[p];"
        "[b][p]paletteuse=dither=floyd_steinberg",
        "-loop",
        "0", // loop forever
        outputPath,
    };

    return std::make_unique<FfmpegEncoder>(ffmpegPath, args, width, height, "GIF encoding");
}

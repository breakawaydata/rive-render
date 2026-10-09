#include "output_gif.hpp"

#include "ffmpeg_process.hpp"

#include <sstream>
#include <stdexcept>

void writeGif(const std::string& outputPath, int width, int height, float fps,
              const std::vector<std::vector<uint8_t>>& frames, const std::string& ffmpegPath)
{
    if (frames.empty())
    {
        throw std::runtime_error("No frames to encode");
    }

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

    runFfmpegWithFrames(ffmpegPath, args, width, height, frames, "GIF encoding");
}

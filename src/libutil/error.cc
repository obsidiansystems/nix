#include "error.hh"

#include <iostream>
#include <optional>
#include "serialise.hh"
#include <sstream>

namespace nix {


const std::string nativeSystem = SYSTEM;

// Traces show the chain of calls in nix code.  If an ErrPos is included the surrounding
// lines of code will print. 
BaseError & BaseError::addTrace(std::optional<ErrPos> e, hintformat hint)
{
    err.traces.push_front(Trace { .pos = e, .hint = hint});
    return *this;
}

// c++ std::exception descendants must have a 'const char* what()' function.
// This stringifies the error and caches it for use by what(), or similarly by msg().
const string& BaseError::calcWhat() const
{
    if (what_.has_value())
        return *what_;
    else {
        err.name = sname();

        std::ostringstream oss;
        oss << err;
        what_ = oss.str();

        return *what_;
    }
}

std::optional<string> ErrorInfo::programName = std::nullopt;

std::ostream& operator<<(std::ostream &os, const hintformat &hf)
{
    return os << hf.str();
}

string showErrPos(const ErrPos &errPos)
{
    if (errPos.line > 0) {
        if (errPos.column > 0) {
            return fmt("(%1%:%2%)", errPos.line, errPos.column);
        } else {
            return fmt("(%1%)", errPos.line);
        }
    }
    else {
        return "";
    }
}

void getCodeLines(NixCode &nixCode)
{
    if (nixCode.errPos.line <= 0)
        return;

    if (nixCode.errPos.origin == foFile) {
        try {
            AutoCloseFD fd = open(nixCode.errPos.file.c_str(), O_RDONLY | O_CLOEXEC);
            if (!fd)
                logError(SysError("opening file '%1%'", nixCode.errPos.file).info());
            else
            {
                // count the newlines.
                int count = 0;
                string line;
                int pl = nixCode.errPos.line - 1;
                do
                {
                    line = readLine(fd.get());
                    ++count;
                    if (count < pl)
                    {
                        ;
                    }
                    else if (count == pl) {
                        nixCode.prevLineOfCode = line;
                    } else if (count == pl + 1) {
                        nixCode.errLineOfCode = line;
                    } else if (count == pl + 2) {
                        nixCode.nextLineOfCode = line;
                        break;
                    }
                } while (true);
            }
        }
        catch (EndOfFile &eof) {
            ;
        }
        catch (std::exception &e) {
            printError("error reading nix file: %s\n%s", nixCode.errPos.file, e.what());
        }
    } else {
        std::istringstream iss(nixCode.errPos.file);
        // count the newlines.
        int count = 0;
        string line;
        int pl = nixCode.errPos.line - 1;

        do
        {
            std::getline(iss, line);
            ++count;
            if (count < pl)
            {
                ;
            }
            else if (count == pl) {
                nixCode.prevLineOfCode = line;
            } else if (count == pl + 1) {
                nixCode.errLineOfCode = line;
            } else if (count == pl + 2) {
                nixCode.nextLineOfCode = line;
                break;
            }

            if (!iss.good())
                break;
        } while (true);
    }
}

// if nixCode contains lines of code, print them to the ostream, indicating the error column.
void printCodeLines(std::ostream &out, const string &prefix, const NixCode &nixCode)
{
    // previous line of code.
    if (nixCode.prevLineOfCode.has_value()) {
        out << std::endl
            << fmt("%1% %|2$5d|| %3%",
            prefix,
            (nixCode.errPos.line - 1),
            *nixCode.prevLineOfCode);
    }

    if (nixCode.errLineOfCode.has_value()) {
        // line of code containing the error.
        out << std::endl
            << fmt("%1% %|2$5d|| %3%",
            prefix,
            (nixCode.errPos.line),
            *nixCode.errLineOfCode);
        // error arrows for the column range.
        if (nixCode.errPos.column > 0) {
            int start = nixCode.errPos.column;
            std::string spaces;
            for (int i = 0; i < start; ++i) {
                spaces.append(" ");
            }

            std::string arrows("^");

            out << std::endl
                << fmt("%1%      |%2%" ANSI_RED "%3%" ANSI_NORMAL,
                prefix,
                spaces,
                arrows);
        }
    }

    // next line of code.
    if (nixCode.nextLineOfCode.has_value()) {
        out << std::endl
            << fmt("%1% %|2$5d|| %3%",
            prefix,
            (nixCode.errPos.line + 1),
            *nixCode.nextLineOfCode);
    }
}

std::ostream& operator<<(std::ostream &out, const ErrorInfo &einfo)
{
    auto errwidth = std::max<size_t>(getWindowSize().second, 20);
    string prefix = "";

    string levelString;
    switch (einfo.level) {
        case Verbosity::lvlError: {
            levelString = ANSI_RED;
            levelString += "error:";
            levelString += ANSI_NORMAL;
            break;
        }
        case Verbosity::lvlWarn: {
            levelString = ANSI_YELLOW;
            levelString += "warning:";
            levelString += ANSI_NORMAL;
            break;
        }
        case Verbosity::lvlInfo: {
            levelString = ANSI_GREEN;
            levelString += "info:";
            levelString += ANSI_NORMAL;
            break;
        }
        case Verbosity::lvlTalkative: {
            levelString = ANSI_GREEN;
            levelString += "talk:";
            levelString += ANSI_NORMAL;
            break;
        }
        case Verbosity::lvlChatty: {
            levelString = ANSI_GREEN;
            levelString += "chat:";
            levelString += ANSI_NORMAL;
            break;
        }
        case Verbosity::lvlVomit: {
            levelString = ANSI_GREEN;
            levelString += "vomit:";
            levelString += ANSI_NORMAL;
            break;
        }
        case Verbosity::lvlDebug: {
            levelString = ANSI_YELLOW;
            levelString += "debug:";
            levelString += ANSI_NORMAL;
            break;
        }
        default: {
            levelString = fmt("invalid error level: %1%", einfo.level);
            break;
        }
    }

    auto ndl = prefix.length() + levelString.length() + 3 + einfo.name.length() + einfo.programName.value_or("").length();
    auto dashwidth = ndl > (errwidth - 3) ? 3 : errwidth - ndl;

    std::string dashes(dashwidth, '-');

    // divider.
    if (einfo.name != "")
        out << fmt("%1%%2%" ANSI_BLUE " --- %3% %4% %5%" ANSI_NORMAL,
            prefix,
            levelString,
            einfo.name,
            dashes,
            einfo.programName.value_or(""));
    else
        out << fmt("%1%%2%" ANSI_BLUE " -----%3% %4%" ANSI_NORMAL,
            prefix,
            levelString,
            dashes,
            einfo.programName.value_or(""));

    bool nl = false;  // intersperse newline between sections.
    if (einfo.errPos.has_value()) {
        switch (einfo.errPos->origin) {
            case foFile: {
                out << prefix << std::endl;
                auto &pos = *einfo.errPos;
                out << prefix << ANSI_BLUE << "at: " << ANSI_YELLOW << showErrPos(pos) << 
                  ANSI_BLUE << " in file: " << ANSI_NORMAL << pos.file;
                break;
            }
            case foString: {
                out << prefix << std::endl;
                out << prefix << ANSI_BLUE << "at: " << ANSI_YELLOW << showErrPos(*einfo.errPos) << 
                  ANSI_BLUE << " from command line argument" << ANSI_NORMAL;
                break;
            }
            case foStdin: {
                out << prefix << std::endl;
                out << prefix << ANSI_BLUE << "at: " << ANSI_YELLOW << showErrPos(*einfo.errPos) << 
                  ANSI_BLUE << " from stdin" << ANSI_NORMAL;
                break;
            }
            default:
                throw Error("invalid FileOrigin in errPos");
        }
        nl = true;
    }

    // description
    if (einfo.description != "") {
        if (nl)
            out << std::endl << prefix;
        out << std::endl << prefix << einfo.description;
        nl = true;
    }

    if (einfo.errPos.has_value()) {
        NixCode nixcode { .errPos = *einfo.errPos }; 
        getCodeLines(nixcode);

        // lines of code.
        if (nixcode.errLineOfCode.has_value()) {
            if (nl)
                out << std::endl << prefix;
            printCodeLines(out, prefix, nixcode);
            nl = true;
        }
    }

    // hint
    if (einfo.hint.has_value()) {
        if (nl)
            out << std::endl << prefix;
        out << std::endl << prefix << *einfo.hint;
        nl = true;
    }

    // traces
    for (auto iter = einfo.traces.rbegin(); iter != einfo.traces.rend(); ++iter)
    {
        try {
            auto pos = *iter->pos;
            if (nl)
                out << std::endl << prefix;
            out << std::endl << prefix;
            out << iter->hint.str() <<  std::endl;
            out << ANSI_BLUE << "at: " << ANSI_YELLOW << showErrPos(pos) << 
              ANSI_BLUE << " in file: " << ANSI_NORMAL << pos.file  << std::endl;
            nl = true;
            NixCode nc { .errPos = pos };
            getCodeLines(nc);
            printCodeLines(out, prefix, nc);
        } catch(const std::bad_optional_access& e) {
            out << iter->hint.str() << std::endl;
        }
    }

    return out;
}
}

#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <ctype.h>
#include <string>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <vector>

using namespace std;

int process_command(string command, bool isChild=false);
int preprocess_command(string& command, string cmd, int* fds);
int setupPipe(string& command, int* f);
void cd(string& command);
void ff(string& command);
void ff_recur(string& fname, string& path);
void ls(string& command);
void print(const string str, int fd=1);
void print(size_t len, char* str, int fd=1);
void print_prompt();
void pwd(string& command);
void redirectIO(string& command, int* fds);
void ResetCanonicalMode(int fd, struct termios *savedattributes);
void SetNonCanonicalMode(int fd, struct termios *savedattributes);
bool runBg(string& command);
char** parse(const string& command);


int main(int argc, char *argv[]){
    struct termios SavedTermAttributes;
    char RXChar;
    SetNonCanonicalMode(STDIN_FILENO, &SavedTermAttributes);
    string command = "";

    // history has "" empty string at the start
    // it's always there, new history additions are added after it
    vector<string> history;
    history.push_back("");
    const int HISTORY_SIZE_MAX = 11; // one extra for the empty string
    int history_position = 0;

    // tab, arrow keys,...
    // check if some escape chars are not handled correctly
    // or if some non-escape chars need to be ignored
    int escape_char_timer = 0;
    print_prompt();
    while (1) {
        read(STDIN_FILENO, &RXChar, 1);
        if (RXChar == 0x04) { // C-d
            print(&RXChar, 1); // write out typed char
            break;
        } else {
            if (RXChar == 0x0A) {
                // Enter pressed, end of command
                print(1, &RXChar); // write out typed char
                if (process_command(command) == -1)
                    break;

                history.insert(history.begin()+1, command);
                if (history.size() > HISTORY_SIZE_MAX) {
                    history.pop_back();
                }

                print_prompt();
                command = "";
            } else if (RXChar == 0x7F || RXChar == 0x7E) {
                // Backspace or Delete
                if (command.length() > 0) {
                    command = command.substr(0, command.length() - 1);
                    print("\b \b"); // move caret back, delete char, move caret back
                } else {
                    print("\a"); // sound the bell
                }
            } else if (RXChar == 0x41 || RXChar == 0x42) {
                if (RXChar == 0x41) {
                    history_position++;
                    if (history_position > history.size() - 1) {
                        history_position = (int)history.size() - 1;
                        print("\a"); // sound the bell
                    }
                } else {
                    history_position--;
                    if (history_position < 0) {
                        history_position = 0;
                        print("\a"); // sound the bell
                    }
                }

                // erase currently displayed command
                for (int i = 0; i < command.length(); i++) {
                    print("\b \b");
                }
                // current command is now one from history
                command = history[history_position];
                // write the new command
                print(command);
            } else {
                // escape char starts, ignore next 3 chars
                if (RXChar == 0x1B) {
                    escape_char_timer = 3;
                }

                if (escape_char_timer > 0) {
                    escape_char_timer--;
                } else {
                    print(1, &RXChar); // write out typed char
                    command += RXChar;
                }
            }
        }
    }

    // collect and terminate all zombies
    // https://stackoverflow.com/questions/19461744/make-parent-wait-for-all-child-processes-to-finish
    while (wait(NULL) != -1);
    ResetCanonicalMode(STDIN_FILENO, &SavedTermAttributes);
    return 0;
}


bool runBg(string& command) {
    size_t pos1 = 0, pos2 = 0;
    // not found &
    if ((pos1=command.find('&')) == string::npos)
        return false;

    // & is not last char (besides ' ')
    if ((pos2=command.find_first_not_of(' ', pos1+1)) != string::npos)
        return false;

    command.erase(pos1, 1);
    if (fork() == 0)
        process_command(command, true);

    return true;
}


// isChild has default value of false.
// return -1 to exit
int process_command(string command, bool isChild) {
    size_t pos = 0;
    int rtn = 0, fds[2] = {-1, -1}, f[2];

    if (runBg(command)) {
        return rtn;
    }

    fds[0] = setupPipe(command, f);
    pos = command.find_first_not_of(' ');

    // get rid of leading spaces in command
    // if not empty string (or zeros)
    if (pos != string::npos) {
        command = command.substr(pos, command.length()-pos);
        if ((rtn = preprocess_command(command, "pwd", fds)) == 1) {
            pwd(command);
        } else if ((rtn = preprocess_command(command, "cd", fds)) == 1) {
            cd(command);
        } else if ((rtn = preprocess_command(command, "ls", fds)) == 1) {
            ls(command);
        } else if ((rtn = preprocess_command(command, "ff", fds)) == 1) {
            ff(command);
        } else if ((rtn = preprocess_command(command, "exit", fds)) == 1) {
            rtn = -1; // flag to exit
        } else { // try execvp()
            redirectIO(command, fds);
            if ((pos=command.find_first_not_of(' ')) != string::npos) {
                pid_t pid = fork();
                if (pid == 0) {
                    char** args = parse(command);
                    if (execvp(args[0], args) == -1)
                        print("Failed to execute " + command + "\n", 2);

                    exit(EXIT_SUCCESS);
                }

                waitpid(pid, NULL, 0);
            }

            rtn = 1;
        }

        if (rtn == 2 || rtn == 0) print("Failed to execute " + command + "\n", 2);
    }

    if (isChild) exit(EXIT_SUCCESS);
    if (fds[0] != -1) {
        close(STDIN_FILENO);
        dup2(fds[0], STDIN_FILENO);
    }

    if (fds[1] != -1) {
        close(STDOUT_FILENO);
        dup2(fds[1], STDOUT_FILENO);
    }

    // no prompt when exiting
    return rtn;
}


// Setup pipelines between commands. Fork on finding a pipe. Attach child's
// stdout to parent's stdin.
// Return value is parents' stdins' copies (Only the initial shell's stdin
// needs to be restored).
//
// source:
// https://www.geeksforgeeks.org/pipe-system-call/
// http://web.cse.ohio-state.edu/~mamrak.1/CIS762/pipes_lab_notes.html
int setupPipe(string& command, int* f) {
    size_t pos = 0;
    int saved_STDIN;
    string subcommand = "";

    pos = command.find_last_of('|');
    if (pos != string::npos) {
        if (pipe(f) < 0) {
            print("pipe failed", 2);
            exit(EXIT_FAILURE);
        }

        pid_t pid = fork();
        if (pid == 0) {
            if (pos != 0)
                subcommand = command.substr(0, pos);

            close(f[0]);
            dup2(f[1], STDOUT_FILENO);
            close(f[1]);
            process_command(subcommand, true);
            exit(EXIT_SUCCESS);
        }

        if (pos+1 < command.length())
            command = command.substr(pos+1, command.length());
        else
            command = "";

        close(f[1]);
        saved_STDIN = dup(STDIN_FILENO);
        dup2(f[0], STDIN_FILENO);
        close(f[0]);
        waitpid(pid, NULL, 0); // wait for child to finish execution and close pipe
        return saved_STDIN;
    }

    return -1;
}


// fds is an int array of length 2 with initial value of {-1, -1}. It will be
// used to restore stdin and stdout. fds[0] will be replaced with a new fd if
// stdin is redirected, fds[1] will if stdout is.
//
// return : 0 on fail validation, 1 on success, 2 on not matching command
// source:
// https://stackoverflow.com/questions/9084099/re-opening-stdout-and-stdin-file-descriptors-after-closing-them
int preprocess_command(string& command, string cmd, int* fds) {
    // starting chars match
    if (command.substr(0, cmd.length()) == cmd) {
        // "lss"
        if (command.length() > cmd.length() &&
            command.substr(0, cmd.length()+1) != cmd+" ")
            return 0;

        redirectIO(command, fds);
        return 1; // success
    }

    return 2; // no match
}


void redirectIO(string& command, int* fds) {
    size_t pos1=0, pos2=0, pos3 = 0;
    string fname;
    // ls>      fiiiiile1  > file2
    //   ^pos1  ^pos2    ^pos3
    while ((pos1=command.find('<')) != string::npos) {
        if (fds[0] == -1) //dup original stdin
            fds[0] = dup(STDIN_FILENO);
        // ls <
        if ((pos2=command.find_first_not_of(' ', pos1+1)) == string::npos) {
            command.erase(pos1, 1);
            dup2(fds[0], STDIN_FILENO);
            fds[0] = -1;
            break;
        } else {
            // if filename were to be last chars of command
            if ((pos3=command.find_first_of(" <>", pos2)) == string::npos)
                pos3 = command.length();

            fname = command.substr(pos2, pos3-pos2);
            command.erase(pos1, pos3-pos1);
            int infd = open(fname.c_str(), O_RDONLY);
            dup2(infd, STDIN_FILENO);
            close(infd);
        }
    }

    while ((pos1=command.find('>')) != string::npos) {
        if (fds[1] == -1) //dup original stdin
            fds[1] = dup(STDOUT_FILENO);
        // ls >
        if ((pos2=command.find_first_not_of(' ', pos1+1)) == string::npos) {
            command.erase(pos1, 1);
            dup2(fds[1], STDOUT_FILENO);
            fds[1] = -1;
            break;
        } else {
            // if filename were to be last chars of command
            // ls >      fileeeeee
            //    ^pos1  ^pos2   ^pos3
            if ((pos3=command.find_first_of(" <>", pos2)) == string::npos)
                pos3 = command.length();

            fname = command.substr(pos2, pos3-pos2);
            command.erase(pos1, pos3-pos1);
            // -rw-r--r--
            int infd = open(fname.c_str(), O_WRONLY|O_CREAT,
                            S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
            dup2(infd, STDOUT_FILENO);
            close(infd);
        }
    }
}


void ResetCanonicalMode(int fd, struct termios *savedattributes) {
    tcsetattr(fd, TCSANOW, savedattributes);
}


void SetNonCanonicalMode(int fd, struct termios *savedattributes) {
    struct termios TermAttributes;

    // Make sure stdin is a terminal.
    if(!isatty(fd)){
        fprintf (stderr, "Not a terminal.\n");
        exit(0);
    }

    // Save the terminal attributes so we can restore them later.
    tcgetattr(fd, savedattributes);

    // Set the funny terminal modes.
    tcgetattr (fd, &TermAttributes);
    TermAttributes.c_lflag &= ~(ICANON | ECHO); // Clear ICANON and ECHO.
    TermAttributes.c_cc[VMIN] = 1;
    TermAttributes.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSAFLUSH, &TermAttributes);
}


//print for strings
//fd specifies output file descriptor
//default is 1 for stdout
void print(string str, int fd) {
    write(fd, str.c_str(), str.length());
}


//print for char arrays(or char)
//len argument specifies length to output
//fd specifies output file descriptor
//default is 1 for stdout
void print(size_t len, char* str, int fd) {
    write(fd, str, len);
}


void print_prompt() {
    size_t buffer_size = 1000;
    char buffer[buffer_size];
    getcwd(buffer, buffer_size);
    string str = string(buffer);
    if (str.length() > 16) {
        size_t cutoff = str.find_last_of("/");
        str = str.substr(cutoff, str.length() - cutoff);
        str += "% ";
        str.insert(0, "/...");
    } else
        str += "% ";

    print(str);
}


//Recursive call used by command ff. Initially called by ff()
void ff_recur(string& fname, string& path) {
    DIR* dir;
    struct dirent *ent;
    struct stat sb;
    string tempPath = "";
    dir = opendir(path.c_str());

    if (dir) {
        while ((ent = readdir(dir))) {
            tempPath = path + "/" + string(ent->d_name);
            if (stat(tempPath.c_str(), &sb) == 0) {
                if (sb.st_mode & S_IFDIR) {
                    if (string(ent->d_name) != "." && string(ent->d_name) != "..") {
                        pid_t pid = fork();
                        if (pid == 0) {
                            ff_recur(fname, tempPath);
                            exit(EXIT_SUCCESS);
                        } // if child process

                        waitpid(pid, NULL, 0);
                    } // if not current or parent dir
                } else {
                    if (string(ent->d_name) == fname) {
                        print(tempPath);
                        print("\n");
                    } // found match
                } // else not dir
            } else {
                print("stat error\n", 2);
                exit(EXIT_FAILURE);
            } // else stat failed
        } // while still in directory stream

        closedir(dir);
    } // if dir opened
    else
        print("Failed to open directory \"" + path + "\"\n", 2);
}


// not handling file name with space in between
// source:
void ff(string& command) {
    size_t pos1 = 0, pos2 = 0;
    string path = ".", fname = "";

    // invalid commands like "ff " should be rerutned
    if ((pos1 = command.find_first_not_of(' ', 3)) == string::npos) {
        print("ff command requires a filename!\n", 2);
        return;
    }
    // ff  filename   path
    //     ^pos1   ^pos2
    //                ^pos1
    pos2 = command.find_first_of(' ', pos1);
    fname = command.substr(pos1, pos2-pos1);

    if ((pos1 = command.find_first_not_of(' ', pos2)) != string::npos) {
        if ((pos2 = command.find_first_of(' ', pos1)) == string::npos)
            pos2 = command.length();

        path = command.substr(pos1, pos2-pos1);
    } // if specified path

    ff_recur(fname, path);
}


void ls(string& command) {
    size_t buffer_size = 1000;
    char buffer[buffer_size];

    // get argument
    // must have space at the end and be a valid directory
    // multiple spaces between ls and argument is ok
    // spaces after ls with no argument also ok
    // additional argument after first one, but separated by spaces is ok too
    // argument can be a folder or path inside current directory
    // or a FULL path(including outside of current directory)
    string argument = ""; // current directory by default
    // remove "ls"
    command = command.substr(2, command.length() - 2);
    size_t space_index = command.find_first_not_of(' ');
    if (space_index != string::npos) {
        // remove everything up to argument start
        command = command.substr(space_index, command.length() - space_index);

        space_index = command.find_first_of(' ');
        if (space_index != string::npos) {
            // remove everything after argument
            command = command.substr(0, space_index);
        }

        argument = command;
    }

    // https://www.ibm.com/support/knowledgecenter/en/SSLTBW_2.3.0/com.ibm.zos.v2r3.bpxbd00/rtread.htm
    // try to use argument as full path first
    DIR* directory = opendir(argument.c_str());
    if (directory == NULL) {
        // try to append argument as folder name to current directory path
        getcwd(buffer, buffer_size);
        argument = "/" + argument;
        argument = buffer + argument;
        directory = opendir(argument.c_str());
    }


    if (directory == NULL) {
        print("Failed to open directory ", 2);
        print(argument, 2);
        print("\n", 2);
    } else {
        struct dirent* entry;

        while (true) {
            entry = readdir(directory);
            struct stat entry_stat;
            string modeval = "";

            if (entry == NULL) {
                break;
            } else {
                // https://stackoverflow.com/questions/8812959/how-to-read-linux-file-permission-programmatically-in-c-c/46436636getcwd(buffer, buffer_size);
                stat(entry->d_name, &entry_stat);
                mode_t permissions = entry_stat.st_mode;
                if (entry_stat.st_mode & S_IFDIR) {
                    modeval += 'd';
                } else {
                    modeval += '-';
                }
                modeval += (permissions & S_IRUSR) ? "r" : "-";
                modeval += (permissions & S_IWUSR) ? "w" : "-";
                modeval += (permissions & S_IXUSR) ? "x" : "-";
                modeval += (permissions & S_IRGRP) ? "r" : "-";
                modeval += (permissions & S_IWGRP) ? "w" : "-";
                modeval += (permissions & S_IXGRP) ? "x" : "-";
                modeval += (permissions & S_IROTH) ? "r" : "-";
                modeval += (permissions & S_IWOTH) ? "w" : "-";
                modeval += (permissions & S_IXOTH) ? "x" : "-";
                modeval += " ";

                print(modeval);
                print(entry->d_name);
                print("\n");
            }
        }
        closedir(directory);
    }
}


// source:
// chdir() https://linux.die.net/man/3/chdir
// stat() http://man7.org/linux/man-pages/man2/fstat.2.html
// https://stackoverflow.com/questions/146924/how-can-i-tell-if-a-given-path-is-a-directory-or-a-file-c-c
// https://stackoverflow.com/questions/9493234/chdir-to-home-directory
// checked for corner cases:
void cd(string& command) {
    size_t pos = 0;
    string path = "";
    struct stat sb;
    int rtn = 0;
    // ommit spaces between chars
    pos = command.find_first_not_of(' ', 3);

    // no path follows "cd ", cd to home dir
    if (pos == string::npos) {
        if (getenv("HOME"))
            rtn = chdir(getenv("HOME"));
    } else {
        path = command.substr(pos, command.length()-pos);
        rtn = chdir(path.c_str());
    }

    if (rtn == -1) {
        if (!path.empty() && stat(path.c_str(), &sb) == 0) {
            // check if path points to directory
            if (!(sb.st_mode & S_IFDIR))
                print(path + " not a directory!\n", 2);
        } else
            print("Error changing directory.\n", 2);
    }
}


// source:
void pwd(string& command) {
    size_t buffer_size = 1000;
    char buffer[buffer_size];
    if (getcwd(buffer, buffer_size) == NULL) {
        print("getcwd() failed: No such file or directory\n");
    } else {
        print(strlen(buffer), buffer);
        print("\n");
    }
}


// parse command with spaces as delimiter.
// return dynamically allocated char* args[] should be freed after use.
// last element of args is NULL.
char** parse(const string& command) {
    size_t pos1 = 0, pos2 = 0;
    int i = 0;
    bool flag = true;

    for (i = 0; (pos1=command.find(' ', pos1+1))!=string::npos; i++);
    char** args = (char**) malloc((i+2)*sizeof(char*));
    string temp;
    i = 0;
    pos1 = 0;
    while (flag) {
        if ((pos2=command.find_first_of(' ', pos1)) == string::npos) {
            pos2 = command.length();
            flag = false;
        }

        temp = command.substr(pos1, pos2-pos1);
        args[i] = (char*) malloc((temp.length()+1)*sizeof(char));
        strcpy(args[i], temp.c_str());
        if ((pos1 = command.find_first_not_of(' ', pos2)) == string::npos)
            flag = false;

        i++;
    }

    args[i] = NULL;
    return args;
}

import re
import os
import socket
import logging
import subprocess
import smtplib
from typing import Dict
from pathlib import Path
from datetime import datetime
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText
from email.mime.base import MIMEBase
from email import encoders
from encryption_utils import decrypt_data

# Set global configuration values
SMTP_SERVER = 'smtp-mail.outlook.com'
SMTP_PORT = 587

# Set global indentation, line count, iteration values and List of all module names
SEQUENCE_LENGTH = 3  # Minimum number of lines in a sequence to consider it for refactoring
REPETITION_THRESHOLD = 3  # Determine the threshold for suggesting refactoring as a function
ALLOWED_CHAR_COUNT = 85
OPERATORS = ['->', '<<', '>>', '==', '!=', '<=', '>=', '&&', '||', '+=', '-=', '*=', '&=', '|=', '^=', '+', '-', '*', '/', '%', '=', '<', '>', '&', '|', ',', '?']
FORMAT_SPECIFIERS = ["%c", "%d", "%e", "%E", "%f", "%g", "%G", "%i", "%ld", "%li", "%lf", "%Lf", "%lu", "%lli", "%lld", "%llu", "%o", "%p", "%s", "%u", "%x", "%X", "%n", "%%"]
EXPORT_FUNCTIONS = ["STATUS hmc7043IfInit(", "STATUS hmc7043InitDev(", 
                    "STATUS hmc7043OutChEnDis(", "STATUS hmc7043ChDoSlip(",
                    "STATUS hmc7043SetSysrefMode(", "STATUS hmc7043SysrefSwPulseN(",
                    "STATUS hmc7043GetAlarm(", "STATUS hmc7043GetAlarms(",
                    "STATUS hmc7043ClearAlarms(", "STATUS hmc7043RegRead(",
                    "STATUS hmc7043RegWrite("]
RESERVED_TYPES = [
    'int', 'short', 'long', 'long long', 'float', 'double', 'long double', 'char', 'wchar_t', 'char16_t', 
    'char32_t', 'bool', 'void', 'enum', 'struct', 'union', 'CKDST_DEV', 'CKDST_DEV_MASK', 'CKDST_FREQ_HZ', 
    'HMC7043_REG', 'HMC7043_PRD_ID', 'HMC7043_REG_READ', 'HMC7043_REG_WRITE', 'Hmc7043_dev_io_if', 
    'HMC7043_DEV_CLKIN_DIV', 'Hmc7043_dev_in_sup', 'HMC7043_DEV_GPI_SUP', 'HMC7043_DEV_GPO_SUP', 
    'HMC7043_DEV_OUTPUT_MODE', 'HMC7043_SREF_MODE', 'HMC7043_SREF_NPULSES', 'Hmc7043_dev_alarms', 
    'HMC7043_CH_MODE', 'HMC7043_CH_DRV_MODE', 'HMC7043_CH_CML_INT_TERM', 'HMC7043_CH_IDLE0', 
    'HMC7043_CH_OUT_SEL', 'Hmc7043_ch_sup', 'HMC7043_CH_MASK', 'Hmc7043_app_dev_params', 'SYS_TIME', 
    'SYS_TIME_NS', 'SYS_TIME_EXT_TICKS', 'SYS_TIME_EXT_SRC', 'SYS_CODE_ERR_HANDLING', 'CODE_ERROR_ID', 
    'FUNCPTR', 'SYS_CODE_ERR_APP_HOOK', 'SOCKET', 'IP_ADDR_V4', 'SYS_THREAD_PRI', 'Sys_thread_ctl_params', 
    'SYS_THREAD_OPTS', 'Sys_thread_args', 'SYS_THREAD_FUNC', 'HSYS_THREAD', 'Sys_thread_per_serv_args', 
    'SYS_LOG_LEVEL', 'Sys_log_params', 'SYS_LOG_EXT_HANDLER', 'SYS_RT_THROTTLE_CTL', 'Sys_sched_params', 
    'SYS_SIG_MASK', 'Sys_signal_handling', 'Sys_util_params', 'SYS_RESOURCE_DELETE_FUNC', 'Utl_ffind', 
    'UTL_FFIND_OPTS', 'HUTL_MUTEX', 'HUTL_QUEUE', 'UTL_Q_STAT', 'Utl_ffind_impl', 'INT8', 'UINT8', 
    'INT16', 'UINT16', 'INT32', 'UINT32', 'INT64', 'UINT64', 'INT128', 'UINT128', 'ULONG', 'UINT4PTR', 
    'PHYSICAL_ADDRESS', 'REAL32', 'REAL64', 'Bool', 'UINT8_Bool', 'STATUS', 'UINT32_ATOMIC', 
    'UINT64_ATOMIC', 'DS7505_DEV_MASK', 'const char'
]
UI_VARIABLES = ['unsigned', 'UINT8', 'UINT32', 'UINT64', 'HMC7043_REG', 'HMC7043_PRD_ID', 'CKDST_DEV', 'CKDST_DEV_MASK', 'CKDST_FREQ_HZ']
# List of known keywords to ignore
KNOWN_KEYWORDS = {
    'const', 'static', 'extern', 'if', 'for', 'while', 'switch', 'case', 'default', 'break', 'continue', 'return', 'goto',
    'else', 'define', 'pragma', 'sizeof', 'typedef', 'do', 'void', 'volatile', 'register', 'restrict', 'inline', 'enum', 'struct', 'union'
}
class ScriptAnalyzer:
    def __init__(self, script_path, recipient_email, encrypted_sender_email, encrypted_sender_password, encryption_key):
        self.script_path = Path(script_path)
        self.recipient_email = recipient_email
        self.sender_email = decrypt_data(encrypted_sender_email, encryption_key).decode()
        self.sender_password = decrypt_data(encrypted_sender_password, encryption_key).decode()
        self.log_file = self.get_log_file_name()
        self.encryption_key = encryption_key
        self.global_declarations = []
        self.RESERVED_TYPES = set(RESERVED_TYPES)  # Use a set for efficient membership checking
        self.counts = {
            'line_length_limit_check': 0,
            'excess_whitespace_check': 0,
            'operator_spacing_check': 0,
            'pointer_naming_check': 0,
            'address_print_check': 0,
            'unsigned_print_check': 0,
            'comment_check': 0,
            'hex_value_check': 0,
            'variable_declarations_check': 0,
            'naming_conventions_check': 0,
            'unsigned_logic_check': 0,
			'replace_name_check': 0,
            'spacing_between_routines_check': 0,
			'brace_placement_check': 0,
            'consistency_check': 0,
        }
        logging.basicConfig(filename=self.log_file, level=logging.INFO,
                            format='%(asctime)s - %(levelname)s - %(message)s')

        def filter_out_http_requests(record):
            server_ip = socket.gethostbyname(socket.gethostname())
            message = record.getMessage()
            if "GET /upload" in message and "HTTP/1.1" in message:
                return False  # Do not log messages containing "GET /upload HTTP/1.1"
            if f"{server_ip} - -" in message:  # Replace the hardcoded IP address with the server's IP address
                return False  # Do not log messages containing the server's IP address
            return True  # Log all other messages

        # Add the filter to the logger
        logger = logging.getLogger(__name__)
        logger.addFilter(filter_out_http_requests)         
        # Initialize error count
        self.error_count = 0
        self.pointer_error_count = 0

    def get_log_file_name(self):
        current_datetime = datetime.now().strftime("%H-%M-%S-on-%d-%m-%Y")
        log_folder = self.script_path.parent / "Logs"
        log_folder.mkdir(parents=True, exist_ok=True)  # Create Logs folder if it doesn't exist
        os.chmod(log_folder, 0o777)  # Set permission to 777
        log_file_name = f"Logs-{self.script_path.stem}-at-{current_datetime}.log"
        return log_folder / log_file_name

    def run_analysis(self):
        try:
            # Print start of script analysis
            print(f"Starting Script Analysis of {self.script_path.stem}.")
            # Creating Log with Analysis in Log Directory
            logging.info(f"Starting Script Analysis of {self.script_path.stem}.")
            
            # Check if the file is a header file or a source file
            file_extension = self.script_path.suffix.lower()
            if file_extension == ".h":
                try:
                    # Check script indentation
                    self.check_name_replace()
                except Exception as e:
                    logging.error(f"Error during Name Convention check: {str(e)}")
                    
                try:
                    # Check number of characters in each line
                    self.check_line_length_limit()
                except Exception as e:
                    logging.error(f"Error during verification of number of characters in each line: {str(e)}")

                try:
                    # Check Comment 
                    self.check_comments()
                except Exception as e:
                    logging.error(f"Error in Comment convention check: {str(e)}")
                                            
            elif file_extension == ".c":
                # # Check for mandatory #include directive
                # self.check_include_directive()
                        
                try:
                    # Extract Global Variables and Datatypes
                    self.extract_global_declarations()
                except Exception as e:
                    logging.error(f"Error during Globally declared Datatypes and Variables in line: {str(e)}")
        
                try:
                    # Check number of characters in each line
                    self.check_line_length_limit()
                except Exception as e:
                    logging.error(f"Error during verification of number of characters in each line: {str(e)}")

                try:
                    # Check Brace placement for Control Statements
                    self.check_brace_placement()
                except Exception as e:
                    logging.error(f"Error during Brace Placement Check for Control Statements: {str(e)}")
                
                try:
                    # Check Script Indentation
                    self.check_variable_declaration()
                except Exception as e:
                    logging.error(f"Variable Declaration check: {str(e)}")
                
                try:
                    # Check Naming Convention
                    self.check_naming_conventions()
                except Exception as e:
                    logging.error(f"Error during naming convention check: {str(e)}")
                    
                try:
                    # Check Line Spacing betwen Functions
                    self.check_spacing_between_routines()
                except Exception as e:
                    logging.error(f"Error Line spacing check between Routines: {str(e)}")

                try:
                    # Check Hex Value Convention
                    self.check_hex_values()
                except Exception as e:
                    logging.error(f"Error during Hex Value convention check: {str(e)}")

                try:
                    # Check Comment Lines
                    self.check_comments()
                except Exception as e:
                    logging.error(f"Error in Comment convention check: {str(e)}")

                try:
                    # Check Consistency
                    self.check_consistency()
                except Exception as e:
                    logging.error(f"Error during Consistency Check: {str(e)}")

                try:
                    # Check Whitespace Usage
                    self.check_excess_whitespace()
                except Exception as e:
                    logging.error(f"Error during whitespace check: {str(e)}")
                    
                try:
                    # Check Unsigned Variable Print Format
                    self.check_unsigned_variables()
                except Exception as e:
                    logging.error(f"Error during Unsigned Variable Print Format check: {str(e)}")

                try:
                    # Check Operator Spacing Format Check
                    self.check_operator_spacing()
                except Exception as e:
                    logging.error(f"Error during Unsigned Variable Print Format check: {str(e)}")

                try:
                    # Check Unsigned variables logic check
                    self.check_unsigned_logic()
                except Exception as e:
                    logging.error(f"Error during Unsigned variables logic check: {str(e)}")

                try:
                    # Check Address print usage
                    self.check_address_print_format()
                except Exception as e:
                    logging.error(f"Error during Address Data-Type check: {str(e)}")

            # Print summary of the analysis results
            print(f"Script Analysis of {self.script_path.stem} Completed.")
            # Creating Log with Analysis in Log Directory
            logging.info(f"Script Analysis of {self.script_path.stem} Completed.")

            # Add summary table to log
            self.add_summary_to_log()

            # Email the log file
            sender_email = self.sender_email
            sender_password = self.sender_password
            recipient_email = self.recipient_email
            attachment_path = self.log_file
            send_email(sender_email, sender_password, recipient_email, attachment_path, self.counts, self.script_path.stem)

        except Exception as e:
            logging.error(f"Error during analysis: {str(e)}")
            self.error_count += 1
            logging.error(f"Error count: {self.error_count}")  # Log the error count

    def add_summary_to_log(self):
        summary = "\n\n----------------------------------------\n"
        # Initialize a variable to check if any issues were found
        issues_found = False

        value = any(val > 0 for val in self.counts.values())
        if value:
            summary += "\n    Summary of Issues observed:\n"
            summary += "-----------------------------------\n"
            summary += "\tCheck\t\t\t\t\t Count\n"
            summary += "-----------------------------------\n"
            
            for check, count in self.counts.items():
                if count >= 1:
                    summary += f" {check.ljust(30)}{count}\n"
                    issues_found = True

        if not issues_found:
            # If no issues were found, print the required message
            summary += " No Issues observed after Analyzing the Script\n"

        summary += "----------------------------------------\n"
        with open(self.log_file, 'a') as log_file:
            log_file.write(summary)
    
    def is_valid_type_name(self, type_name):
        """Check if a type name is valid."""
        # Check if the type name is not a known keyword and not in the RESERVED_TYPES list
        if type_name in KNOWN_KEYWORDS or type_name in self.RESERVED_TYPES:
            return False
        # Further validation rules can be added here
        return True
    
    def extract_global_declarations(self):
        # Patterns for various global declarations
        patterns = [
            re.compile(r'typedef\s+struct\s+\{[^}]*\}\s*(\w+)\s*;', re.DOTALL | re.IGNORECASE),
            re.compile(r'LOCAL\s+struct\s+\{[^}]*\}\s*(\w+)\s*;', re.DOTALL | re.IGNORECASE),
            re.compile(r'typedef\s+enum\s+\{[^}]*\}\s*(\w+)\s*;', re.DOTALL | re.IGNORECASE),
            re.compile(r'typedef\s+union\s+\{[^}]*\}\s*(\w+)\s*;', re.DOTALL | re.IGNORECASE),
            re.compile(r'static\s+(\w+)\s+\w+\s*;', re.IGNORECASE),
            re.compile(r'extern\s+(\w+)\s+\w+\s*;', re.IGNORECASE)
        ]

        # Pattern for function declarations
        func_pattern = re.compile(r'(\w+)\s+\w+\s*\((\w+\s+\w+(,\s*\w+\s+\w+)*)\)', re.IGNORECASE)

        try:
            with open(self.script_path, "r") as script_file:
                content = script_file.read()

            initial_reserved_types = set(self.RESERVED_TYPES)

            for pattern in patterns:
                matches = pattern.findall(content)
                for match in matches:
                    type_name = match.strip()
                    if self.is_valid_type_name(type_name):
                        self.RESERVED_TYPES.add(type_name)
                        logging.info(f"Added new reserved type: {type_name}")

            # Check for function declarations
            func_matches = func_pattern.findall(content)
            for match in func_matches:
                return_type, func_name, params = match
                if self.is_valid_type_name(return_type):
                    self.RESERVED_TYPES.add(return_type)
                    logging.info(f"Added new reserved return type from function: {return_type}")

                param_list = params.split(',')
                for param in param_list:
                    param_parts = param.strip().split()
                    if len(param_parts) > 1:
                        type_name = param_parts[0]
                        if self.is_valid_type_name(type_name):
                            self.RESERVED_TYPES.add(type_name)
                            logging.info(f"Added new reserved type from function parameter: {type_name}")

            # Find and log the newly added types
            new_reserved_types = self.RESERVED_TYPES - initial_reserved_types
            if new_reserved_types:
                logging.info(f"Newly appended reserved types: {', '.join(new_reserved_types)}")

            logging.info("Global declaration extraction completed")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during global declaration extraction: {str(e)}")
    
    def check_include_directive(self):
        try:
            with open(self.script_path, "r") as script_file:
                lines = script_file.readlines()

            in_multiline_comment = False
            include_directive_found = False
            code_found = False
            for line_number, line in enumerate(lines, start=1):
                stripped_line = line.strip()
                if not stripped_line:
                    continue
                if "/*" in stripped_line:
                    in_multiline_comment = True
                if in_multiline_comment:
                    if "*/" in stripped_line:
                        in_multiline_comment = False
                    continue
                if stripped_line.startswith("//"):
                    continue
                if stripped_line.startswith("#include "):
                    if code_found:
                        logging.error(f"Include Directive: '#include ' directive found after code at line {line_number}.")
                        self.counts['include_directive_check'] += 1  # Increment the count
                    else:
                        include_directive_found = True
                else:
                    code_found = True

            if not include_directive_found:
                logging.error("Include Directive: Mandatory '#include ' directive missing at the beginning of the file.")
                self.counts['include_directive_check'] += 1  # Increment the count

            logging.info(f"Include Directive: Check completed - Error Count: {self.counts['include_directive_check']}")

        except FileNotFoundError:
            logging.error(f"Exception Error: File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Exception Error: Issues during include directive check: {str(e)}")

    def check_name_replace(self):
        try:
            with open(self.script_path, "r") as script_file:
                for line_number, line in enumerate(script_file, start=1):
                    if "EEPROM_H" in line:
                        logging.info(f"Naming convention: Found EEPROM_H on line {line_number}. Please Replace it with _eeprom_h_.")
                        self.counts['replace_name_check'] += 1

            logging.info(f"Naming convention: Check completed - Count: {self.counts['replace_name_check']}")

        except FileNotFoundError:
            logging.error(f"Exception Error: File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Exception Error: Issues during Name Convention check: {str(e)}")

    def check_line_length_limit(self):
        try:
            with open(self.script_path, "r") as script_file:
                lines = script_file.readlines()

            for line_number, line in enumerate(lines, start=1):
                line_length = len(line)
                if line_length > ALLOWED_CHAR_COUNT:
                    logging.info(f"Line Length Limit: Exceeded {ALLOWED_CHAR_COUNT} Characters: Line {line_number} has {line_length} characters.")
                    self.counts['line_length_limit_check'] += 1

                    # Word or symbol containing the 85th Character
                    char_at_85 = line[ALLOWED_CHAR_COUNT - 1]
                    words = line.split()
                    word_with_85th_char = next((word for word in words if char_at_85 in word), None)
                    if word_with_85th_char:
                        logging.info(f"Line {line_number}: The word containing the 85th character '{char_at_85}' is '{word_with_85th_char}'")

                    # Last non-space/non-tab word in the line
                    stripped_line = line.rstrip()
                    last_word = stripped_line.split()[-1] if stripped_line else None
                    if last_word:
                        # Position of the last non-space/non-tab character of the last word
                        last_char_position = stripped_line.rfind(last_word[-1]) + 1
                        logging.info(f"Line {line_number}: The last non-space/non-tab word is '{last_word}' and the position of the last character '{last_word[-1]}' is {last_char_position}")

            logging.info(f"Line length limit check completed - Error Count: {self.counts['line_length_limit_check']}")

        except FileNotFoundError:
            logging.error(f"Exception Error: File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Exception Error: Issues during line length limit check: {str(e)}")

    def check_variable_declaration(self):
        global_variable_declaration = False
        in_control_data_block = False
        block_open_count = 0
        initialized_variables = set()
        function_pattern = re.compile(r'\s*(EXPORT\s+|LOCAL\s+|STATIC\s+)?\w+\s+\w+\s*\([^)]*\)\s*{?$')

        try:
            with open(self.script_path, "r") as script_file:
                content = script_file.readlines()

            for line_number, line in enumerate(content, start=1):
                line = line.strip()

                # Skip empty lines
                if not line:
                    continue

                # Skip comments
                if line.startswith("//") or line.startswith("/*"):
                    continue

                # Process #include and control data block
                if line.startswith("#include"):
                    global_variable_declaration = True

                if global_variable_declaration and line.startswith("/* Control Data */"):
                    global_variable_declaration = False
                    in_control_data_block = True

                # Count block open and close
                if "{" in line:
                    block_open_count += 1
                if "}" in line:
                    block_open_count -= 1

                # Skip lines containing "(" or ")" to avoid function and argument checks
                if "(" in line or ")" in line:
                    continue

                # Check for variable declarations
                if "=" in line and line.strip().endswith(";"):
                    var_name = line.split("=")[0].strip().split()[-1]
                    if "->" in line or "." in line:
                        continue  # Skip struct member assignments

                    if block_open_count > 0 or var_name not in initialized_variables:
                        initialized_variables.add(var_name)
                    else:
                        logging.info(f"Variable Initialization: Variable '{var_name}' already initialized: Line {line_number} '{line.strip()}'")
                        continue  # Skip logging as error if already initialized

                # Check for function declarations
                if function_pattern.match(line):
                    continue  # Skip function declaration or definition lines

                # Check for reserved types in variable declarations
                if not global_variable_declaration and not in_control_data_block:
                    if any(line.strip().startswith(data_type) for data_type in self.RESERVED_TYPES):
                        var_name = line.split()[-1].replace(";", "")
                        if var_name not in initialized_variables and not line.endswith(";") and "=" not in line:
                            logging.info(f"Variable Declaration: Declaration of variable '{var_name}' needs to be at the beginning of the block: Line {line_number} '{line.strip()}'")
                            self.counts['variable_declarations_check'] += 1

            logging.info(f"Variable Declaration: Check completed - Error Count: {self.counts['variable_declarations_check']}")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during variable declaration check: {str(e)}")
            
    def check_variable_initialization(self):
        global_variable_declaration = False
        function_pattern = re.compile(r'\w+\s+\w+\(.*\)\s*{?$')
        try:
            with open(self.script_path, "r") as script_file:
                for line_number, line in enumerate(script_file, start=1):
                    line = line.strip()

                    if line.startswith("#include"):
                        global_variable_declaration = True

                    if global_variable_declaration and line.startswith("/* Control Data */"):
                        global_variable_declaration = False

                    if "{" in line and line.strip().endswith("{"):
                        global_variable_declaration = False  # Reset global_variable_declaration for local variable check
                        continue  # Skip block opening

                    if not global_variable_declaration:
                        if any(line.strip().startswith(data_type) for data_type in RESERVED_TYPES):
                            if not function_pattern.match(line):
                                if "=" in line:
                                    logging.info(f"Variable Initialization: Avoid initializing variables at declaration: Line {line_number} '{line.strip()}'")
                                    self.counts['variable_initialization_check'] += 1

            logging.info(f"Variable Initialization: Check completed - Error Count: {self.counts['variable_initialization_check']}")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during variable initialization check: {str(e)}")

    def check_spacing_between_routines(self):
        empty_lines_count = 0
        comment_block = False

        try:
            with open(self.script_path, 'r') as file:
                for line_number, line in enumerate(file, start=1):
                    line = line.strip()

                    if not line:
                        empty_lines_count += 1
                        continue

                    if empty_lines_count == 4 and line.startswith('/*******************************************************************************'):
                        empty_lines_count = 0
                        comment_block = True
                        continue

                    if comment_block and line.endswith('*******************************************************************************/'):
                        empty_lines_count = 4
                        comment_block = False
                        continue

                    if empty_lines_count == 4 and line.startswith('int main()'):
                        found_main = True
                        break

                    if not comment_block and empty_lines_count == 4 and line.startswith('* - name:'):
                        logging.info(f"Expected 4 empty lines before '{line}' at line {line_number}.")
                        self.counts['spacing_between_routines_check'] += 1
                        empty_lines_count = 0
                        continue

                    if not comment_block and empty_lines_count == 4 and line.endswith('}'):
                        logging.info(f"Expected 4 empty lines after function block at line {line_number}.")
                        self.counts['spacing_between_routines_check'] += 1
                        empty_lines_count = 0
                        continue

                    empty_lines_count = 0

            logging.info(f"Spacing between routines check completed - Error Count: {self.counts['spacing_between_routines_check']}")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during spacing between routines check: {str(e)}")

    def check_brace_placement(self):
        try:
            with open(self.script_path, "r") as script_file:
                lines = script_file.readlines()

            control_structure_stack = []
            control_structures = ["if", "else if", "else", "switch", "for", "while", "do", "case", "default"]
            function_patterns = ["EXPORT STATUS", "LOCAL STATUS"]
            in_multiline_comment = False

            for line_number, line in enumerate(lines, start=1):
                stripped_line = line.strip()
                if not stripped_line:
                    continue

                # Eliminate multiline comments
                if "/*" in stripped_line:
                    in_multiline_comment = True
                if in_multiline_comment:
                    if "*/" in stripped_line:
                        in_multiline_comment = False
                    continue

                # Eliminate single line comments
                if "//" in stripped_line:
                    continue

                # Ignore lines containing typedef and lines where # and define appear together (ignoring spaces)
                if stripped_line.startswith("typedef") or "#define" in stripped_line.replace(" ", ""):
                    continue

                for control_structure in control_structures:
                    if control_structure in stripped_line:
                        if "{" in stripped_line and not stripped_line.rstrip("/*").endswith("{"):
                            logging.info(f"Opening brace should be on the same line as the {control_structure} statement: line {line_number}")
                            self.counts['brace_placement_check'] += 1
                            control_structure_stack.append(control_structure)
                        elif control_structure_stack and not stripped_line.startswith("{"):
                            logging.info(f"Opening brace should be on the same line as the {control_structure} statement: line {line_number}")
                            self.counts['brace_placement_check'] += 1

                if any(pattern in line for pattern in function_patterns):
                    continue

                if control_structure_stack and stripped_line.startswith("}"):
                    last_structure = control_structure_stack.pop()
                    if not stripped_line.endswith("}"):
                        logging.info(f"Closing brace should be on a new line after the {last_structure} block: line {line_number}")
                        self.counts['brace_placement_check'] += 1

                if "}" in stripped_line and not stripped_line.startswith("}"):
                    logging.info(f"Closing brace should be on a new line: line {line_number}")
                    self.counts['brace_placement_check'] += 1

            logging.info(f"Brace placement check completed - Error Count: {self.counts['brace_placement_check']}")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during brace placement check: line {line_number} - {str(e)}")

    def check_naming_conventions(self):
        try:
            with open(self.script_path, "r") as script_file:
                lines = script_file.readlines()

            in_multiline_comment = False
            for line_number, line in enumerate(lines, start=1):
                # Check if the line starts a multi-line comment
                if "/*" in line:
                    in_multiline_comment = True

                # Skip checking if the line is within a comment
                if in_multiline_comment or line.startswith("//"):
                    # Check if the line ends a multi-line comment
                    if "*/" in line:
                        in_multiline_comment = False
                    continue

                # Check for Prefix in STATUS Function name
                for func_name in EXPORT_FUNCTIONS:
                    if func_name in line and not line.strip().startswith("EXPORT " + func_name):
                        logging.info(f"Function name should be prefixed with 'EXPORT': Line {line_number} '{line.strip()}'")
                        self.counts['naming_conventions_check'] += 1

                # Check for local prefix
                if not any(func_name in line for func_name in EXPORT_FUNCTIONS):
                    if line.startswith("STATUS ") and "(" in line and ")" in line:
                        # Extract function name and arguments
                        func_parts = line.split(" ")[1].split("(")
                        if len(func_parts) > 1:
                            func_name = func_parts[0]
                            if func_name not in EXPORT_FUNCTIONS:
                                logging.info(f"Function name should be prefixed with 'LOCAL': Line {line_number} '{line.strip()}'")
                                self.counts['naming_conventions_check'] += 1

            logging.info(f"Naming convention check completed - Error Count: {self.counts['naming_conventions_check']}")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during naming convention check: {str(e)}")

    def check_operator_spacing(self):
        try:
            with open(self.script_path, "r") as script_file:
                lines = script_file.readlines()

            # Compile patterns
            single_line_comment_pattern = re.compile(r'//.*')
            multi_line_comment_pattern = re.compile(r'/\*.*?\*/')
            multi_line_comment_start = re.compile(r'/\*')
            multi_line_comment_end = re.compile(r'\*/')
            preprocessor_directive_pattern = re.compile(r'^\s*#(include|pragma)')
            operator_pattern = re.compile(r'({0})'.format('|'.join(re.escape(op) for op in OPERATORS if op not in ["++", "--"])))
            format_specifier_pattern = re.compile('|'.join(re.escape(fs) for fs in FORMAT_SPECIFIERS))
            correct_pointer_pattern = re.compile(r'\*p[A-Z]')  # Pattern to match correct pointers
            incorrect_pointer_pattern = re.compile(r'\*[a-oq-zA-Z]')  # Pattern to match incorrect pointers

            # Pattern to match pointer declarations
            pointer_declaration_pattern = re.compile(r'\*\w+')

            error_count = 0
            pointer_error_count = 0
            in_multi_line_comment = False
            lines_with_errors = {}  # Dictionary to store line numbers and their respective operators

            for line_number, line in enumerate(lines, start=1):
                # Skip multi-line comments
                if in_multi_line_comment:
                    if multi_line_comment_end.search(line):
                        in_multi_line_comment = False
                    continue

                if multi_line_comment_start.search(line):
                    in_multi_line_comment = True
                    continue

                # Skip single-line comments and preprocessor directives
                if single_line_comment_pattern.search(line) or preprocessor_directive_pattern.search(line):
                    continue

                # Skip lines with format specifiers
                if format_specifier_pattern.search(line):
                    continue

                # Remove multi-line comments
                line = multi_line_comment_pattern.sub('', line)

                # Skip increment and decrement operators
                if '++' in line or '--' in line:
                    continue

                # Check for pointer naming errors
                incorrect_pointer_match = incorrect_pointer_pattern.search(line)
                if incorrect_pointer_match and not correct_pointer_pattern.search(line):
                    incorrect_pointer = incorrect_pointer_match.group()
                    logging.info(f"Pointer Error: Incorrect pointer naming '{incorrect_pointer}' at line {line_number}.")
                    pointer_error_count += 1
                    continue

                # Skip pointer declarations
                if pointer_declaration_pattern.search(line):
                    continue

                # Check for operator spacing errors
                operators_found = operator_pattern.findall(line)
                for operator in operators_found:
                    # Check if there are spaces around the operator
                    spaced_operator_pattern = r'(?<!\S)' + re.escape(operator) + r'(?!\S)'
                    if not re.search(spaced_operator_pattern, line):
                        if line_number not in lines_with_errors:
                            lines_with_errors[line_number] = set()
                        lines_with_errors[line_number].add(operator)

            # Log operator spacing errors
            for line_number, operators in sorted(lines_with_errors.items()):
                operator_list = ', '.join(operators)
                logging.info(f"Operator Error: Improper spacing for operators {operator_list} in line {line_number}.")
                error_count += 1

            # Log summary of errors
            logging.info(f"Operator spacing check completed - Error Count: {error_count}")
            logging.info(f"Pointer naming check completed - Error Count: {pointer_error_count}")

            # Update error counts
            self.counts['operator_spacing_check'] = error_count
            self.counts['pointer_naming_check'] = pointer_error_count

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during operator spacing check: {str(e)}")

    def check_hex_values(self):
        try:
            with open(self.script_path, "r") as script_file:
                lines = script_file.readlines()

            in_multiline_comment = False
            for line_number, line in enumerate(lines, start=1):
                # Check if the line starts a multi-line comment
                if "/*" in line:
                    in_multiline_comment = True

                # Skip checking if the line is within a comment
                if in_multiline_comment or line.startswith("//"):
                    # Check if the line ends a multi-line comment
                    if "*/" in line:
                        in_multiline_comment = False
                    continue

                hex_values = re.findall(r'0x[A-F0-9]+', line)
                for hex_value in hex_values:
                    if any(char.isupper() for char in hex_value):
                        logging.info(f"Avoid capital letters in hex values at line {line_number}: {hex_value}")
                        self.counts['hex_value_check'] += 1

            logging.info(f"Hex value check completed - Error Count: {self.counts['hex_value_check']}")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")

    def check_comments(self):
        consecutive_comments = []
        in_multiline_comment = False
        in_string = False
        try:
            with open(self.script_path, "r") as script_file:
                for line_number, line in enumerate(script_file, start=1):
                    line = line.strip()

                    # Check for comments within strings
                    if '"' in line:
                        quote_indices = [i for i, char in enumerate(line) if char == '"']
                        for i in range(0, len(quote_indices), 2):
                            if i + 1 < len(quote_indices):  # Ensure there's a closing quote
                                string_content = line[quote_indices[i] + 1:quote_indices[i + 1]]
                                if "//" in string_content:
                                    logging.info(f"Avoid using // and use /*..*/: Line {line_number} '{line}'")
                                    self.counts['comment_check'] += 1

                    # Check for single-line comments (//) followed by a comment
                    if "//" in line and not in_string and not in_multiline_comment:
                        comment_index = line.index("//")
                        comment_part = line[comment_index + 2:].strip()
                        if comment_part:  # Check if there's a comment after //
                            consecutive_comments.append((line_number, line))
                    else:
                        if len(consecutive_comments) > 1:
                            start_line = consecutive_comments[0][0]
                            end_line = consecutive_comments[-1][0]
                            logging.info(f"Avoid using // and use /*..*/ for Comments between lines: {start_line} to {end_line}")
                            self.counts['comment_check'] += 1
                        elif len(consecutive_comments) == 1:
                            line_number, comment_line = consecutive_comments[0]
                            logging.info(f"Avoid using // and use /*..*/: Line {line_number} '{comment_line}'")
                            self.counts['comment_check'] += 1
                        consecutive_comments = []

                    # Check for comments enclosed by /* and */
                    if "/*" in line and not in_string and not in_multiline_comment:
                        if "*/" not in line:
                            in_multiline_comment = True
                    if "*/" in line and in_multiline_comment:
                        in_multiline_comment = False

                    # Check for start and end of string
                    if '"' in line:
                        quote_count = line.count('"')
                        if quote_count % 2 == 1:
                            in_string = not in_string

                # Check for any remaining consecutive comments at the end of the file
                if len(consecutive_comments) > 1:
                    start_line = consecutive_comments[0][0]
                    end_line = consecutive_comments[-1][0]
                    logging.info(f"Comment Check: Avoid using // and use /*..*/ for Comments between Lines: {start_line} and {end_line}")
                    self.counts['comment_check'] += 1
                elif len(consecutive_comments) == 1:
                    line_number, comment_line = consecutive_comments[0]
                    logging.info(f"Comment Check: Avoid using // and use /*..*/: Line {line_number} '{comment_line}'")
                    self.counts['comment_check'] += 1

                logging.info(f"Comment based check completed - Error Count: {self.counts['comment_check']}")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during comment check: {str(e)}")

    def check_consistency(self):
        try:
            with open(self.script_path, "r") as script_file:
                lines = script_file.readlines()

            in_multiline_comment = False
            line_endings = set()
            for line_number, line in enumerate(lines, start=1):
                # Check if the line starts a multi-line comment
                if "/*" in line:
                    in_multiline_comment = True

                # Skip checking if the line is within a comment
                if in_multiline_comment or line.startswith("//"):
                    # Check if the line ends a multi-line comment
                    if "*/" in line:
                        in_multiline_comment = False
                    continue

                try:
                    if '\r\n' in line:
                        line_endings.add('CRLF')
                    elif '\n' in line:
                        line_endings.add('LF')
                except IndexError as ie:
                    logging.error(f"IndexError at line {line_number}: {line.strip()} - {str(ie)}")
                    self.counts['consistency_check'] += 1
                except ValueError as ve:
                    logging.error(f"ValueError at line {line_number}: {line.strip()} - {str(ve)}")
                    self.counts['consistency_check'] += 1
                except Exception as e:
                    logging.error(f"Unexpected error at line {line_number}: {line.strip()} - {str(e)}")
                    self.counts['consistency_check'] += 1

            if len(line_endings) > 1:
                logging.info('Inconsistent line endings found in the script. Use either CRLF (line break "\r\n") or LF (line break "\n"), not both.')
                self.counts['consistency_check'] += 1

            logging.info(f"Consistency check completed - Error Count: {self.counts['consistency_check']}")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except IOError as ioe:
            logging.error(f"I/O error occurred: {str(ioe)}")
        except Exception as e:
            logging.error(f"Error during consistency check: {str(e)}")

    def check_excess_whitespace(self):
        try:
            with open(self.script_path, "r") as script_file:
                lines = script_file.readlines()

            in_multiline_comment = False
            for line_number, line in enumerate(lines, start=1):
                # Check if the line starts a multi-line comment
                if "/*" in line:
                    in_multiline_comment = True

                # Skip checking if the line is within a comment or within double quotes
                if in_multiline_comment or line.startswith("//"):
                    # Check if the line ends a multi-line comment
                    if "*/" in line:
                        in_multiline_comment = False
                    continue

                matches = re.findall(r'(\S+)\s{2,}(\S+)', line)
                for match in matches:
                    logging.info(f"Whitespace Check: Excess whitespace detected between '{match[0]}' and '{match[1]}': Line {line_number}.")
                    self.counts['excess_whitespace_check'] += 1

            logging.info(f"Whitespace Check: Excess whitespace check completed - Error Count: {self.counts['excess_whitespace_check']}")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during excess whitespace check: {str(e)}")

    def check_unsigned_logic(self):
        try:
            with open(self.script_path, "r") as script_file:
                lines = script_file.readlines()

            # Regular expression to match variable declarations
            declaration_pattern = r'\b(?:' + '|'.join(UI_VARIABLES) + r')\s+(\w+)\b'
            variable_declarations = re.findall(declaration_pattern, '\n'.join(lines))

            # Regular expression to match checks for unsigned variables
            check_pattern = r'\b({0})\s*<=?\s*0'.format('|'.join(variable_declarations))

            error_count = 0  # Initialize error count

            for line_number, line in enumerate(lines, start=1):
                match = re.search(check_pattern, line)
                if match:
                    variable = match.group(1)  # Get the variable name from the match
                    logging.error(f"Unsigned Logic: Please replace unsigned {variable} <= 0: Line {line_number} with !{variable} instead.")
                    error_count += 1  # Increment error count

            logging.info(f"Unsigned Logic: Unsigned logic check completed - Error Count: {error_count}")
            self.counts['unsigned_logic_check'] += error_count

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during unsigned logic check: {str(e)}")
            
    def check_unsigned_variables(self):
        try:
            with open(self.script_path, "r") as script_file:
                lines = script_file.readlines()

            # Regular expression to match variable declarations
            declaration_pattern = r'\b(?:' + '|'.join(UI_VARIABLES) + r')\s+(\w+)\b'
            variable_declarations = re.findall(declaration_pattern, '\n'.join(lines))

            # Regular expression to match sysLog statements
            syslog_pattern = r'sysLog\("(.*?)(%d|%i)(.*?)",\s*(.*?)\)'        

            error_count = 0  # Initialize error count

            for line_number, line in enumerate(lines, start=1):
                # Check for nBytes variable declaration
                nbytes_pattern = r'\b(' + '|'.join(UI_VARIABLES) + r')\s+nBytes\b'
                nbytes_match = re.search(nbytes_pattern, line)
                if nbytes_match:
                    data_type = nbytes_match.group(1)
                    if data_type != 'unsigned':
                        logging.info(f"Unsigned Variable: Please replace '{data_type}' with 'unsigned' to declare 'nBytes': Line {line_number}")
                        error_count += 1
                        self.counts['unsigned_print_check'] += 1

                for match in re.finditer(syslog_pattern, line):
                    format_specifier = match.group(2)
                    arguments = match.group(4).split(',')  # Split arguments in sysLog
                    for arg in arguments:
                        if arg.strip() in variable_declarations and format_specifier != '%u':
                            logging.info(f"Unsigned Variable: Incorrect Format used to print unsigned variable {arg.strip()}: Line {line_number}: Please Use %u instead.")
                            error_count += 1  # Increment error count
                            self.counts['unsigned_print_check'] += 1

            logging.info(f"Unsigned Variable: Unsigned variables check completed - Error Count: {error_count}")

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during unsigned variables check: {str(e)}")

    def check_address_print_format(self):
        try:
            # Regular expression to match sysLog, printf, sprintf, fprintf, scanf statements with addr or address
            print_pattern = r'(sysLog|printf|sprintf|fprintf|scanf)\s*\(".*?(addr|address).*?%([-+ 0#]{0,3})(\d+|\*)?(\.\d+|\.\*)?([hl]{0,2}x)'
            # Regular expression to match any % specifier
            any_format_pattern = r'%([-+ 0#]{0,3})(\d+|\*)?(\.\d+|\.\*)?([hl]{0,2}[diufFeEgGpoaAcsn])'

            error_count = 0  # Initialize error count

            with open(self.script_path, "r") as script_file:
                for line_number, line in enumerate(script_file, start=1):
                    if re.search(print_pattern, line, re.IGNORECASE):
                        match = re.search(any_format_pattern, line)
                        if match:
                            incorrect_format = match.group(0)  # Get the incorrect format specifier
                            logging.info(f"Address Format: Incorrect format specifier for address: Line {line_number}: Replace {incorrect_format} with %x or its variations.")
                            error_count += 1  # Increment error count

            logging.info(f"Address Format: Address print format check completed - Error Count: {error_count}")
            self.counts['address_print_check'] += error_count

        except FileNotFoundError:
            logging.error(f"File not found: {self.script_path}")
        except Exception as e:
            logging.error(f"Error during address print format check: {str(e)}")

def send_email(sender_email, sender_password, recipient_email, attachment_path, counts, script_name):
    # Create a multipart message
    message = MIMEMultipart()
    message['From'] = sender_email
    message['To'] = recipient_email
    recipient_user = recipient_email.split('@')[0]
    recipient_user = recipient_user.capitalize()

    # Get the current date and format it as desired
    current_date = datetime.now().strftime('%d-%m-%Y')
    subject = f"Script Analysis Log - {script_name} - {current_date}"
    message['Subject'] = subject

    # Add body to email
    body = body = f"Hello {recipient_user},<br><br>"
    body += "Please find attached the log file for the script analysis.<br>"
    body += "<b><font size='4.5' color='#000000'>File Type: </font></b> .c File<br><br>"
    body += "<u><b><font size='4.5' color='#000000'>Summary:</font></b></u><br><br>"

    # Create a table for counts with added CSS for better styling
    table = "<table style='border-collapse: collapse; border: 4px solid black; width: 50%; background-color: #F0F0F0; margin-left: auto; margin-right: auto;'>"
    table += "<tr><th style='border: 2px solid black; padding: 15px; text-align: left; background-color: #ADD8E6; color: black;'><b>Code Quality Metric</b></th><th style='border: 2px solid black; padding: 15px; text-align: center; background-color: #ADD8E6; color: black; padding-left: 10px; padding-right: 10px;'><b>Anomaly Frequency</b></th></tr>"
    

    # Define a dictionary to map the check names to more understandable terms
    check_names = {
        'line_length_limit_check': 'Character Count Check',
        'brace_placement_check': 'Brace Placement Check',
        'variable_declarations_check': 'Variable Declaration Check',
        # 'variable_initialization_check': 'Variable Initialization Check',
        'spacing_between_routines_check': 'Line Spacing Check Functions',
        'hex_value_check': 'Hex Value Check',
        'comment_check': 'Comment Convention Check',
        'naming_conventions_check': 'Naming Standards Assessment',
        'replace_name_check': 'Name Check eeprom',
        'consistency_check': 'Code Uniformity Check',
        'excess_whitespace_check': 'Whitespace Reduction Analysis',
        'operator_spacing_check': 'Operator Spacing Check',
        'pointer_naming_check': 'Pointer Naming Convention',
        'unsigned_print_check':'Unsigned Print Condition',
        'unsigned_logic_check':'Unsigned Logic Check',
        'address_print_check':'Hex-Address Print Check'
    }

    for check, count in counts.items():
        # Replace the check name with the corresponding term in the email body
        check_name = check_names.get(check, check)
        table += f"<tr><td style='border: 2px solid black; padding: 15px; text-align: left;'>{check_name}</td><td style='border: 2px solid black; padding: 15px; text-align: center;'>{count}</td></tr>"  # Reduce the cell size of the counts column, change the border color to black, increase the padding to 15px, and left-align the text in the first column
    table += "</table>"

    # Adding Table to the Message body
    body += table

    # Add a couple of line breaks and the desired text
    body += "<br><br>Please Refer to the Attached Log for the detailed Analysis<br><br>Regards<br>ScriptAnalyzer-QA<br>"
    
    message.attach(MIMEText(body, 'html'))

    # Open the file to be sent  
    filename = os.path.basename(attachment_path)
    attachment = open(attachment_path, "rb")

    # Instance of MIMEBase and named as p
    p = MIMEBase('application', 'octet-stream')

    # To change the payload into encoded form
    p.set_payload((attachment).read())

    # encode into base64
    encoders.encode_base64(p)

    p.add_header('Content-Disposition', "attachment; filename= %s" % filename)  # Use filename instead of attachment_path

    # attach the instance 'p' to instance 'msg'
    message.attach(p)

    # Create SMTP session for sending the mail
    session = smtplib.SMTP(SMTP_SERVER, SMTP_PORT)
    session.starttls()  # Enable security
    session.login(sender_email, sender_password)  # Login
    text = message.as_string()
    session.sendmail(sender_email, recipient_email, text)  # Send email
    session.quit()  # Terminate the session

# Main program
if __name__ == "__main__":
    # Analyze the script
    script_analyzer = ScriptAnalyzer(script_path, recipient_email, encrypted_sender_email, encrypted_sender_password, encryption_key)
    script_analyzer.run_analysis()
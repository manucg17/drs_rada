import os
from cryptography.fernet import Fernet

def generate_key():
    return Fernet.generate_key()

def encrypt_data(data, key):
    cipher_suite = Fernet(key)
    encrypted_data = cipher_suite.encrypt(data)
    return encrypted_data

def decrypt_data(encrypted_data, key):
    cipher_suite = Fernet(key)
    decrypted_data = cipher_suite.decrypt(encrypted_data)
    return decrypted_data

def save_key():
    key = generate_key()
    current_dir = os.path.dirname(os.path.abspath(__file__))
    key_path = os.path.join(current_dir, 'encryption.key')
    with open(key_path, 'wb') as key_file:
        key_file.write(key)
    print("Key saved to:", key_path)


def load_key():
    current_dir = os.path.dirname(os.path.abspath(__file__))
    key_path = os.path.join(current_dir, 'encryption.key')
    print("Current directory:", current_dir)
    print("Key path:", key_path)
    try:
        with open(key_path, 'rb') as key_file:
            return key_file.read()
    except FileNotFoundError:
        print("Key file not found.")
        return None

save_key()
load_key()
